/* Copyright (c) 2015, 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "binding.h"
#include "coverage.h"
#include "byte-order.h"
#include "ct-zone.h"
#include "encaps.h"
#include "flow.h"
#include "ha-chassis.h"
#include "lflow.h"
#include "local_data.h"
#include "lport.h"
#include "chassis.h"
#include "lib/bundle.h"
#include "openvswitch/poll-loop.h"
#include "lib/uuid.h"
#include "ofctrl.h"
#include "openvswitch/list.h"
#include "openvswitch/hmap.h"
#include "openvswitch/match.h"
#include "openvswitch/ofp-actions.h"
#include "openvswitch/ofpbuf.h"
#include "openvswitch/vlog.h"
#include "openvswitch/ofp-parse.h"
#include "ovn-controller.h"
#include "lib/chassis-index.h"
#include "lib/mcast-group-index.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"
#include "ovn/actions.h"
#include "if-status.h"
#include "physical.h"
#include "pinctrl.h"
#include "openvswitch/shash.h"
#include "simap.h"
#include "smap.h"
#include "sset.h"
#include "util.h"
#include "vswitch-idl.h"
#include "hmapx.h"

VLOG_DEFINE_THIS_MODULE(physical);

COVERAGE_DEFINE(physical_run);

/* Datapath zone IDs for connection tracking and NAT */
struct zone_ids {
    int ct;                     /* MFF_LOG_CT_ZONE. */
    int dnat;                   /* MFF_LOG_DNAT_ZONE. */
    int snat;                   /* MFF_LOG_SNAT_ZONE. */
};

static void
load_logical_ingress_metadata(const struct sbrec_port_binding *binding,
                              const struct zone_ids *zone_ids,
                              size_t n_encap_ips,
                              const char **encap_ips,
                              struct ofpbuf *ofpacts_p,
                              bool load_encap_id);
static int64_t get_vxlan_port_key(int64_t port_key);

/* UUID to identify OF flows not associated with ovsdb rows. */
static struct uuid *hc_uuid = NULL;

#define CHASSIS_MAC_TO_ROUTER_MAC_CONJID        100

void
physical_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_bridge);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_ports);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_port);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_interfaces);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_external_ids);

    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_interface);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_mtu);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_ofport);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_external_ids);
}

static void
put_move(enum mf_field_id src, int src_ofs,
         enum mf_field_id dst, int dst_ofs,
         int n_bits,
         struct ofpbuf *ofpacts)
{
    struct ofpact_reg_move *move = ofpact_put_REG_MOVE(ofpacts);
    move->src.field = mf_from_id(src);
    move->src.ofs = src_ofs;
    move->src.n_bits = n_bits;
    move->dst.field = mf_from_id(dst);
    move->dst.ofs = dst_ofs;
    move->dst.n_bits = n_bits;
}

static void
put_resubmit(uint8_t table_id, struct ofpbuf *ofpacts)
{
    struct ofpact_resubmit *resubmit = ofpact_put_RESUBMIT(ofpacts);
    resubmit->in_port = OFPP_IN_PORT;
    resubmit->table_id = table_id;
}

/*
 * For an encap and a chassis, get the corresponding ovn-chassis-id tunnel
 * port.
 */
static struct chassis_tunnel *
get_port_binding_tun(const struct sbrec_encap *remote_encap,
                     const struct sbrec_chassis *chassis,
                     const struct hmap *chassis_tunnels,
                     const char *local_encap_ip)
{
    struct chassis_tunnel *tun = NULL;
    tun = chassis_tunnel_find(chassis_tunnels, chassis->name,
                              remote_encap ? remote_encap->ip : NULL,
                              local_encap_ip);

    if (!tun) {
        tun = chassis_tunnel_find(chassis_tunnels, chassis->name, NULL, NULL);
    }
    return tun;
}

static void
put_encapsulation(enum mf_field_id mff_ovn_geneve,
                  const struct chassis_tunnel *tun,
                  const struct sbrec_datapath_binding *datapath,
                  uint16_t outport, bool is_ramp_switch,
                  struct ofpbuf *ofpacts)
{
    if (tun->type == GENEVE) {
        put_load(datapath->tunnel_key, MFF_TUN_ID, 0, 24, ofpacts);
        put_load(outport, mff_ovn_geneve, 0, 32, ofpacts);
        put_move(MFF_LOG_INPORT, 0, mff_ovn_geneve, 16, 15, ofpacts);
    } else if (tun->type == VXLAN) {
        uint64_t vni = datapath->tunnel_key;
        if (!is_ramp_switch) {
            /* Map southbound 16-bit port key to limited 12-bit range
             * available for VXLAN, which differs for multicast groups. */
            vni |= get_vxlan_port_key(outport) << 12;
        }
        put_load(vni, MFF_TUN_ID, 0, 24, ofpacts);
    } else {
        OVS_NOT_REACHED();
    }
}

static void
put_decapsulation(enum mf_field_id mff_ovn_geneve,
                  const struct chassis_tunnel *tun,
                  struct ofpbuf *ofpacts)
{
    if (tun->type == GENEVE) {
        put_move(MFF_TUN_ID, 0,  MFF_LOG_DATAPATH, 0, 24, ofpacts);
        put_move(mff_ovn_geneve, 16, MFF_LOG_INPORT, 0, 15, ofpacts);
        put_move(mff_ovn_geneve, 0, MFF_LOG_OUTPORT, 0, 16, ofpacts);
    } else if (tun->type == VXLAN) {
        /* Add flows for non-VTEP tunnels. Split VNI into two 12-bit
         * sections and use them for datapath and outport IDs. */
        put_move(MFF_TUN_ID, 12, MFF_LOG_OUTPORT,  0, 12, ofpacts);
        put_move(MFF_TUN_ID, 0, MFF_LOG_DATAPATH, 0, 12, ofpacts);
    } else {
        OVS_NOT_REACHED();
    }
}


static void
put_remote_chassis_flood_encap(struct ofpbuf *ofpacts,
                               enum chassis_tunnel_type type,
                               enum mf_field_id mff_ovn_geneve)
{
    if (type == GENEVE) {
        put_move(MFF_LOG_DATAPATH, 0,  MFF_TUN_ID, 0, 24, ofpacts);
        put_load(0, mff_ovn_geneve, 0, 32, ofpacts);
        put_move(MFF_LOG_INPORT, 0, mff_ovn_geneve, 16, 15, ofpacts);
    } else if (type == VXLAN) {
        put_move(MFF_LOG_INPORT, 0, MFF_TUN_ID,  12, 12, ofpacts);
        put_move(MFF_LOG_DATAPATH, 0, MFF_TUN_ID, 0, 12, ofpacts);
    } else {
        OVS_NOT_REACHED();
    }
}

static void
match_set_chassis_flood_outport(struct match *match,
                                enum chassis_tunnel_type type,
                                enum mf_field_id mff_ovn_geneve)
{
    if (type == GENEVE) {
        /* Outport occupies the lower half of tunnel metadata (0-15). */
        union mf_value value, mask;
        memset(&value, 0, sizeof value);
        memset(&mask, 0, sizeof mask);

        const struct mf_field *mf_ovn_geneve = mf_from_id(mff_ovn_geneve);
        memset(&mask.tun_metadata[mf_ovn_geneve->n_bytes - 2], 0xff, 2);

        tun_metadata_set_match(mf_ovn_geneve, &value, &mask, match, NULL);
    }
}

static void
match_set_chassis_flood_remote(struct match *match, uint32_t index)
{
    match_init_catchall(match);
    match_set_reg(match, MFF_REG6 - MFF_REG0, index);
    /* Match if the packet wasn't already received from tunnel.
     * This prevents from looping it back to the tunnel again. */
    match_set_reg_masked(match, MFF_LOG_FLAGS - MFF_REG0, 0,
                         MLF_RX_FROM_TUNNEL);
}


static void
put_stack(enum mf_field_id field, struct ofpact_stack *stack)
{
    stack->subfield.field = mf_from_id(field);
    stack->subfield.ofs = 0;
    stack->subfield.n_bits = stack->subfield.field->n_bits;
}

/* Split the ofpacts buffer to prevent overflow of the
 * MAX_ACTIONS_BUFSIZE netlink buffer size supported by the kernel.
 * In order to avoid all the action buffers to be squashed together by
 * ovs, add a controller action for each configured openflow.
 */
static void
put_split_buf_function(uint32_t index, uint32_t outport, uint8_t stage,
                       struct ofpbuf *ofpacts)
{
    ovs_be32 values[2] = {
        htonl(index),
        htonl(outport)
    };
    size_t oc_offset =
           encode_start_controller_op(ACTION_OPCODE_SPLIT_BUF_ACTION, false,
                                      NX_CTLR_NO_METER, ofpacts);
    ofpbuf_put(ofpacts, values, sizeof values);
    ofpbuf_put(ofpacts, &stage, sizeof stage);
    encode_finish_controller_op(oc_offset, ofpacts);
}

static const struct sbrec_port_binding *
get_localnet_port(const struct hmap *local_datapaths, int64_t tunnel_key)
{
    const struct local_datapath *ld = get_local_datapath(local_datapaths,
                                                         tunnel_key);
    return ld ? ld->localnet_port : NULL;
}


static bool
has_vtep_port(const struct hmap *local_datapaths, int64_t tunnel_key)
{
    const struct local_datapath *ld = get_local_datapath(local_datapaths,
                                                         tunnel_key);
    return ld != NULL;
}


static struct zone_ids
get_zone_ids(const struct sbrec_port_binding *binding,
             const struct shash *ct_zones)
{
    struct zone_ids zone_ids = {
        .ct = ct_zone_find_zone(ct_zones, binding->logical_port)
    };

    const char *name = smap_get(&binding->datapath->external_ids, "name");
    if (!name) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_ERR_RL(&rl, "Missing name for datapath '"UUID_FMT"'",
                    UUID_ARGS(&binding->datapath->header_.uuid));

        return zone_ids;
    }

    char *dnat = alloc_nat_zone_key(name, "dnat");
    zone_ids.dnat = ct_zone_find_zone(ct_zones, dnat);
    free(dnat);

    char *snat = alloc_nat_zone_key(name, "snat");
    zone_ids.snat = ct_zone_find_zone(ct_zones, snat);
    free(snat);

    return zone_ids;
}

static void
put_remote_port_redirect_bridged(const struct
                                 sbrec_port_binding *binding,
                                 const enum en_lport_type type,
                                 const struct hmap *local_datapaths,
                                 struct local_datapath *ld,
                                 struct match *match,
                                 struct ofpbuf *ofpacts_p,
                                 struct ovn_desired_flow_table *flow_table)
{
        if (type != LP_CHASSISREDIRECT) {
            /* bridged based redirect is only supported for chassisredirect
             * type remote ports. */
            return;
        }

        struct eth_addr binding_mac;
        bool  is_valid_mac = extract_sbrec_binding_first_mac(binding,
                                                             &binding_mac);
        if (!is_valid_mac) {
            return;
        }

        uint32_t ls_dp_key = 0;
        const struct peer_ports *peers;
        VECTOR_FOR_EACH_PTR (&ld->peer_ports, peers) {
            const struct sbrec_port_binding *sport_binding = peers->remote;
            const char *sport_peer_name =
                smap_get(&sport_binding->options, "peer");
            const char *distributed_port =
                smap_get(&binding->options, "distributed-port");

            if (!strcmp(sport_peer_name, distributed_port)) {
                ls_dp_key = sport_binding->datapath->tunnel_key;
                break;
            }
        }

        if (!ls_dp_key) {
            return;
        }

        union mf_value value;
        struct ofpact_mac *src_mac;
        const struct sbrec_port_binding *ls_localnet_port;

        ls_localnet_port = get_localnet_port(local_datapaths, ls_dp_key);
        if (!ls_localnet_port) {
            return;
        }

        src_mac = ofpact_put_SET_ETH_SRC(ofpacts_p);
        src_mac->mac = binding_mac;

        value.be64 = htonll(ls_dp_key);

        ofpact_put_set_field(ofpacts_p, mf_from_id(MFF_METADATA),
                             &value, NULL);

        value.be32 = htonl(ls_localnet_port->tunnel_key);
        ofpact_put_set_field(ofpacts_p, mf_from_id(MFF_REG15),
                             &value, NULL);

        put_resubmit(OFTABLE_LOG_TO_PHY, ofpacts_p);
        ofctrl_add_flow(flow_table, OFTABLE_LOCAL_OUTPUT, 100,
                        binding->header_.uuid.parts[0],
                        match, ofpacts_p, &binding->header_.uuid);

}

static void
match_outport_dp_and_port_keys(struct match *match,
                               uint32_t dp_key, uint32_t port_key)
{
    match_init_catchall(match);
    match_set_metadata(match, htonll(dp_key));
    match_set_reg(match, MFF_LOG_OUTPORT - MFF_REG0, port_key);
}

static struct sbrec_encap *
find_additional_encap_for_chassis(const struct sbrec_port_binding *pb,
                                  const struct sbrec_chassis *chassis_rec)
{
    for (size_t i = 0; i < pb->n_additional_encap; i++) {
        if (!strcmp(pb->additional_encap[i]->chassis_name,
                    chassis_rec->name)) {
            return pb->additional_encap[i];
        }
    }
    return NULL;
}

static struct vector
get_remote_tunnels(const struct sbrec_port_binding *binding,
                   const struct physical_ctx *ctx,
                   const char *local_encap_ip)
{
    struct vector tunnels =
        VECTOR_EMPTY_INITIALIZER(const struct chassis_tunnel *);
    const struct chassis_tunnel *tun;
    if (binding->chassis && binding->chassis != ctx->chassis) {
        tun = get_port_binding_tun(binding->encap, binding->chassis,
                ctx->chassis_tunnels, local_encap_ip);
        if (!tun) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(
                &rl, "Failed to locate tunnel to reach main chassis %s "
                     "for port %s.",
                binding->chassis->name, binding->logical_port);
        } else {
            vector_push(&tunnels, &tun);
        }
    }

    for (size_t i = 0; i < binding->n_additional_chassis; i++) {
        if (binding->additional_chassis[i] == ctx->chassis) {
            continue;
        }
        const struct sbrec_encap *additional_encap;
        additional_encap = find_additional_encap_for_chassis(binding,
                                                             ctx->chassis);
        tun = get_port_binding_tun(additional_encap,
                                   binding->additional_chassis[i],
                                   ctx->chassis_tunnels, local_encap_ip);
        if (!tun) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(
                &rl, "Failed to locate tunnel to reach additional chassis %s "
                     "for port %s.",
                binding->additional_chassis[i]->name, binding->logical_port);
            continue;
        }
        vector_push(&tunnels, &tun);
    }
    return tunnels;
}

static void
put_remote_port_redirect_overlay(const struct sbrec_port_binding *binding,
                                 const enum en_lport_type type,
                                 const struct physical_ctx *ctx,
                                 uint32_t port_key,
                                 struct match *match,
                                 struct ofpbuf *ofpacts_p,
                                 struct ovn_desired_flow_table *flow_table)
{
    /* Setup encapsulation */
    for (size_t i = 0; i < ctx->n_encap_ips; i++) {
        const char *encap_ip = ctx->encap_ips[i];
        struct ofpbuf *ofpacts_clone = ofpbuf_clone(ofpacts_p);

        match_set_reg_masked(match, MFF_LOG_ENCAP_ID - MFF_REG0, i << 16,
                             (uint32_t) 0xFFFF << 16);
        struct vector tuns = get_remote_tunnels(binding, ctx, encap_ip);
        if (!vector_is_empty(&tuns)) {
            bool is_vtep_port = type == LP_VTEP;
            /* rewrite MFF_IN_PORT to bypass OpenFlow loopback check for ARP/ND
             * responder in L3 networks. */
            if (is_vtep_port) {
                put_load(ofp_to_u16(OFPP_NONE), MFF_IN_PORT, 0, 16,
                         ofpacts_clone);
            }

            const struct chassis_tunnel *tun;
            VECTOR_FOR_EACH (&tuns, tun) {
                put_encapsulation(ctx->mff_ovn_geneve, tun, binding->datapath,
                                  port_key, is_vtep_port, ofpacts_clone);
                ofpact_put_OUTPUT(ofpacts_clone)->port = tun->ofport;
            }
            put_resubmit(OFTABLE_LOCAL_OUTPUT, ofpacts_clone);
            ofctrl_add_flow(flow_table, OFTABLE_REMOTE_OUTPUT, 100,
                            binding->header_.uuid.parts[0], match,
                            ofpacts_clone, &binding->header_.uuid);
        }
        vector_destroy(&tuns);
        ofpbuf_delete(ofpacts_clone);
    }
}

static void
put_remote_port_redirect_overlay_ha_remote(
    const struct sbrec_port_binding *binding,
    const enum en_lport_type type,
    struct ha_chassis_ordered *ha_ch_ordered,
    enum mf_field_id mff_ovn_geneve, uint32_t port_key,
    struct match *match, struct ofpbuf *ofpacts_p,
    const struct hmap *chassis_tunnels,
    struct ovn_desired_flow_table *flow_table)
{
    /* Make sure all tunnel endpoints use the same encapsulation,
     * and set it up */
    const struct chassis_tunnel *tun = NULL;
    for (size_t i = 0; i < ha_ch_ordered->n_ha_ch; i++) {
        const struct sbrec_chassis *ch = ha_ch_ordered->ha_ch[i].chassis;
        if (!ch) {
            continue;
        }
        if (!tun) {
            tun = chassis_tunnel_find(chassis_tunnels, ch->name, NULL, NULL);
        } else {
            struct chassis_tunnel *chassis_tunnel =
                chassis_tunnel_find(chassis_tunnels, ch->name, NULL, NULL);
            if (chassis_tunnel &&
                tun->type != chassis_tunnel->type) {
                static struct vlog_rate_limit rl =
                              VLOG_RATE_LIMIT_INIT(1, 1);
                VLOG_ERR_RL(&rl, "Port %s has Gateway_Chassis "
                            "with mixed encapsulations, only "
                            "uniform encapsulations are "
                            "supported.", binding->logical_port);
                return;
            }
        }
    }
    if (!tun) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_ERR_RL(&rl, "No tunnel endpoint found for HA chassis in "
                    "HA chassis group of port %s",
                    binding->logical_port);
        return;
    }

    put_encapsulation(mff_ovn_geneve, tun, binding->datapath, port_key,
                      type == LP_VTEP, ofpacts_p);

    /* Output to tunnels with active/backup */
    struct ofpact_bundle *bundle = ofpact_put_BUNDLE(ofpacts_p);

    for (size_t i = 0; i < ha_ch_ordered->n_ha_ch; i++) {
        const struct sbrec_chassis *ch =
            ha_ch_ordered->ha_ch[i].chassis;
        if (!ch) {
            continue;
        }
        tun = chassis_tunnel_find(chassis_tunnels, ch->name, NULL, NULL);
        if (!tun) {
            continue;
        }
        if (bundle->n_members >= BUNDLE_MAX_MEMBERS) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "Remote endpoints for port beyond "
                         "BUNDLE_MAX_MEMBERS");
            break;
        }
        ofpbuf_put(ofpacts_p, &tun->ofport, sizeof tun->ofport);
        bundle = ofpacts_p->header;
        bundle->n_members++;
    }

    bundle->algorithm = NX_BD_ALG_ACTIVE_BACKUP;
    /* Although ACTIVE_BACKUP bundle algorithm seems to ignore
     * the next two fields, those are always set */
    bundle->basis = 0;
    bundle->fields = NX_HASH_FIELDS_ETH_SRC;
    ofpact_finish_BUNDLE(ofpacts_p, &bundle);

    ofctrl_add_flow(flow_table, OFTABLE_REMOTE_OUTPUT, 100,
                    binding->header_.uuid.parts[0],
                    match, ofpacts_p, &binding->header_.uuid);
}


static struct hmap remote_chassis_macs =
    HMAP_INITIALIZER(&remote_chassis_macs);

/* Maps from a physical network name to the chassis macs of remote chassis. */
struct remote_chassis_mac {
    struct hmap_node hmap_node;
    char *chassis_mac;
    char *chassis_id;
    uint32_t chassis_sb_cookie;
};

static void
populate_remote_chassis_macs(const struct sbrec_chassis *my_chassis,
                             const struct sbrec_chassis_table *chassis_table)
{
    const struct sbrec_chassis *chassis;
    SBREC_CHASSIS_TABLE_FOR_EACH (chassis, chassis_table) {

        /* We want only remote chassis macs. */
        if (!strcmp(my_chassis->name, chassis->name)) {
            continue;
        }

        const char *tokens
            = get_chassis_mac_mappings(&chassis->other_config, chassis->name);

        if (!tokens[0]) {
            continue;
        }

        char *save_ptr = NULL;
        char *token;
        char *tokstr = xstrdup(tokens);

        /* Format for a chassis mac configuration is:
         * ovn-chassis-mac-mappings="bridge-name1:MAC1,bridge-name2:MAC2"
         */
        for (token = strtok_r(tokstr, ",", &save_ptr);
             token != NULL;
             token = strtok_r(NULL, ",", &save_ptr)) {
            char *save_ptr2 = NULL;
            char *chassis_mac_bridge = strtok_r(token, ":", &save_ptr2);
            char *chassis_mac_str = strtok_r(NULL, "", &save_ptr2);
            if (!chassis_mac_str) {
                VLOG_WARN("Parsing of ovn-chassis-mac-mappings failed for: "
                          "\"%s\", the correct format is \"br-name1:MAC1\".",
                          token);
                continue;
            }
            struct remote_chassis_mac *remote_chassis_mac = NULL;
            remote_chassis_mac = xmalloc(sizeof *remote_chassis_mac);
            hmap_insert(&remote_chassis_macs, &remote_chassis_mac->hmap_node,
                        hash_string(chassis_mac_bridge, 0));
            remote_chassis_mac->chassis_mac = xstrdup(chassis_mac_str);
            remote_chassis_mac->chassis_id = xstrdup(chassis->name);
            remote_chassis_mac->chassis_sb_cookie =
                chassis->header_.uuid.parts[0];
        }
        free(tokstr);
    }
}

static void
free_remote_chassis_macs(void)
{
    struct remote_chassis_mac *mac;

    HMAP_FOR_EACH_SAFE (mac, hmap_node, &remote_chassis_macs) {
        hmap_remove(&remote_chassis_macs, &mac->hmap_node);
        free(mac->chassis_mac);
        free(mac->chassis_id);
        free(mac);
    }
}

static void
put_chassis_mac_conj_id_flow(const struct sbrec_chassis_table *chassis_table,
                             const struct sbrec_chassis *chassis,
                             struct ofpbuf *ofpacts_p,
                             struct ovn_desired_flow_table *flow_table)
{
    struct match match;
    struct remote_chassis_mac *mac;

    populate_remote_chassis_macs(chassis, chassis_table);

    HMAP_FOR_EACH (mac, hmap_node, &remote_chassis_macs) {
        struct eth_addr chassis_mac;
        char *err_str = NULL;
        struct ofpact_conjunction *conj;

        if ((err_str = str_to_mac(mac->chassis_mac, &chassis_mac))) {
            free(err_str);
            free_remote_chassis_macs();
            return;
        }

        ofpbuf_clear(ofpacts_p);
        match_init_catchall(&match);


        match_set_dl_src(&match, chassis_mac);

        conj = ofpact_put_CONJUNCTION(ofpacts_p);
        conj->id = CHASSIS_MAC_TO_ROUTER_MAC_CONJID;
        conj->n_clauses = 2;
        conj->clause = 0;
        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 180,
                        mac->chassis_sb_cookie,
                        &match, ofpacts_p, hc_uuid);
    }

    free_remote_chassis_macs();
}

static void
put_replace_chassis_mac_flows(const struct shash *ct_zones,
                              const struct
                              sbrec_port_binding *localnet_port,
                              const struct hmap *local_datapaths,
                              struct ofpbuf *ofpacts_p,
                              ofp_port_t ofport,
                              struct ovn_desired_flow_table *flow_table)
{
    /* Packets arriving on localnet port, could have been routed on
     * source chassis and hence will have a chassis mac.
     * conj_match will match source mac with chassis macs conjunction
     * and replace it with corresponding router port mac.
     */
    struct local_datapath *ld = get_local_datapath(local_datapaths,
                                                   localnet_port->datapath->
                                                   tunnel_key);
    ovs_assert(ld);

    int tag = localnet_port->tag ? *localnet_port->tag : 0;
    struct zone_ids zone_ids = get_zone_ids(localnet_port, ct_zones);

    const struct peer_ports *peers;
    VECTOR_FOR_EACH_PTR (&ld->peer_ports, peers) {
        const struct sbrec_port_binding *rport_binding = peers->remote;
        struct eth_addr router_port_mac;
        char *err_str = NULL;
        struct match match;
        struct ofpact_mac *replace_mac;

        ovs_assert(rport_binding->n_mac == 1);
        if ((err_str = str_to_mac(rport_binding->mac[0], &router_port_mac))) {
            /* Parsing of mac failed. */
            VLOG_WARN("Parsing or router port mac failed for router port: %s, "
                    "with error: %s", rport_binding->logical_port, err_str);
            free(err_str);
            return;
        }
        ofpbuf_clear(ofpacts_p);
        match_init_catchall(&match);

        /* Add flow, which will match on conjunction id and will
         * replace source with router port mac */

        /* Match on ingress port, vlan_id and conjunction id */
        match_set_in_port(&match, ofport);
        match_set_conj_id(&match, CHASSIS_MAC_TO_ROUTER_MAC_CONJID);

        if (tag) {
            match_set_dl_vlan(&match, htons(tag), 0);
        } else {
            match_set_dl_tci_masked(&match, 0, htons(VLAN_CFI));
        }

        /* Actions */

        if (tag) {
            ofpact_put_STRIP_VLAN(ofpacts_p);
        }
        load_logical_ingress_metadata(localnet_port, &zone_ids, 0, NULL,
                                      ofpacts_p, true);
        replace_mac = ofpact_put_SET_ETH_SRC(ofpacts_p);
        replace_mac->mac = router_port_mac;

        /* Resubmit to first logical ingress pipeline table. */
        put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, ofpacts_p);
        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 180,
                        rport_binding->header_.uuid.parts[0],
                        &match, ofpacts_p, &localnet_port->header_.uuid);

        /* Provide second search criteria, i.e localnet port's
         * vlan ID for conjunction flow */
        struct ofpact_conjunction *conj;
        ofpbuf_clear(ofpacts_p);
        match_init_catchall(&match);

        match_set_in_port(&match, ofport);
        if (tag) {
            match_set_dl_vlan(&match, htons(tag), 0);
        } else {
            match_set_dl_tci_masked(&match, 0, htons(VLAN_CFI));
        }

        conj = ofpact_put_CONJUNCTION(ofpacts_p);
        conj->id = CHASSIS_MAC_TO_ROUTER_MAC_CONJID;
        conj->n_clauses = 2;
        conj->clause = 1;
        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 180,
                        rport_binding->header_.uuid.parts[0],
                        &match, ofpacts_p, &localnet_port->header_.uuid);
    }
}

#define VLAN_8021AD_ETHTYPE 0x88a8
#define VLAN_8021Q_ETHTYPE 0x8100

static void
ofpact_put_push_vlan(struct ofpbuf *ofpacts, const struct smap *options, int tag)
{
    const char *ethtype_opt = options ? smap_get(options, "ethtype") : NULL;

    int ethtype = VLAN_8021Q_ETHTYPE;
    if (ethtype_opt) {
      if (!strcasecmp(ethtype_opt, "802.11ad")
          || !strcasecmp(ethtype_opt, "802.1ad")) {
        ethtype = VLAN_8021AD_ETHTYPE;
      } else if (!strcasecmp(ethtype_opt, "802.11q")
                 || !strcasecmp(ethtype_opt, "802.1q")) {
        ethtype = VLAN_8021Q_ETHTYPE;
      } else {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "Unknown port ethtype: %s", ethtype_opt);
      }
      if (!strcasecmp(ethtype_opt, "802.11ad")
          || !strcasecmp(ethtype_opt, "802.11q")) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "Using incorrect value ethtype: %s for either "
                          "802.1q or 802.1ad please correct this value",
                          ethtype_opt);
      }
    }

    struct ofpact_push_vlan *push_vlan;
    push_vlan = ofpact_put_PUSH_VLAN(ofpacts);
    push_vlan->ethertype = htons(ethtype);

    struct ofpact_vlan_vid *vlan_vid;
    vlan_vid = ofpact_put_SET_VLAN_VID(ofpacts);
    vlan_vid->vlan_vid = tag;
    vlan_vid->push_vlan_if_needed = false;
}

static void
put_replace_router_port_mac_flows(const struct physical_ctx *ctx,
                                  const struct
                                  sbrec_port_binding *localnet_port,
                                  struct ofpbuf *ofpacts_p,
                                  ofp_port_t ofport,
                                  struct ovn_desired_flow_table *flow_table)
{
    struct local_datapath *ld = get_local_datapath(ctx->local_datapaths,
                                                   localnet_port->datapath->
                                                   tunnel_key);
    ovs_assert(ld);

    uint32_t dp_key = localnet_port->datapath->tunnel_key;
    uint32_t port_key = localnet_port->tunnel_key;
    int tag = localnet_port->tag ? *localnet_port->tag : 0;
    const char *network = smap_get(&localnet_port->options, "network_name");
    struct eth_addr chassis_mac;

    if (!network) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "Physical network not configured for datapath:"
                     "%"PRId64" with localnet port",
                     localnet_port->datapath->tunnel_key);
        return;
    }

    /* Get chassis mac */
    if (!chassis_get_mac(ctx->chassis, network, &chassis_mac)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        /* Keeping the log level low for backward compatibility.
         * Chassis mac is a new configuration.
         */
        VLOG_DBG_RL(&rl, "Could not get chassis mac for network: %s", network);
        return;
    }

    const struct peer_ports *peers;
    VECTOR_FOR_EACH_PTR (&ld->peer_ports, peers) {
        const struct sbrec_port_binding *rport_binding = peers->remote;
        struct eth_addr router_port_mac;
        struct match match;
        struct ofpact_mac *replace_mac;
        char *cr_peer_name = xasprintf("cr-%s", rport_binding->logical_port);
        if (lport_is_chassis_resident(ctx->sbrec_port_binding_by_name,
                                      ctx->chassis, cr_peer_name)) {
            /* If a router port's chassisredirect port is
             * resident on this chassis, then we need not do mac replace. */
            free(cr_peer_name);
            continue;
        }
        free(cr_peer_name);

        /* Table 65, priority 150.
         * =======================
         *
         * Implements output to localnet port.
         * a. Flow replaces ingress router port mac with a chassis mac.
         * b. Flow appends the vlan id localnet port is configured with.
         */
        ofpbuf_clear(ofpacts_p);

        ovs_assert(rport_binding->n_mac == 1);
        char *err_str = str_to_mac(rport_binding->mac[0], &router_port_mac);
        if (err_str) {
            /* Parsing of mac failed. */
            VLOG_WARN("Parsing or router port mac failed for router port: %s, "
                      "with error: %s", rport_binding->logical_port, err_str);
            free(err_str);
            return;
        }

        /* Replace Router mac flow */
        match_outport_dp_and_port_keys(&match, dp_key, port_key);
        match_set_dl_src(&match, router_port_mac);

        replace_mac = ofpact_put_SET_ETH_SRC(ofpacts_p);
        replace_mac->mac = chassis_mac;

        if (tag) {
            ofpact_put_push_vlan(ofpacts_p, &localnet_port->options, tag);
        }

        ofpact_put_OUTPUT(ofpacts_p)->port = ofport;

        /* Replace the MAC back and strip vlan. In case of l2 flooding
         * traffic (ARP/ND) we need to restore previous state so other ports
         * do not receive the traffic tagged and with wrong MAC. */
        ofpact_put_SET_ETH_SRC(ofpacts_p)->mac = router_port_mac;
        if (tag) {
            ofpact_put_STRIP_VLAN(ofpacts_p);
        }

        ofctrl_add_flow(flow_table, OFTABLE_LOG_TO_PHY, 150,
                        localnet_port->header_.uuid.parts[0],
                        &match, ofpacts_p, &localnet_port->header_.uuid);
    }
}

static void
put_zones_ofpacts(const struct zone_ids *zone_ids, struct ofpbuf *ofpacts_p)
{
    if (zone_ids) {
        if (zone_ids->ct) {
            put_load(zone_ids->ct, MFF_LOG_CT_ZONE, 0, 16, ofpacts_p);
        }
        if (zone_ids->dnat) {
            put_load(zone_ids->dnat, MFF_LOG_DNAT_ZONE, 0, 32, ofpacts_p);
        }
        if (zone_ids->snat) {
            put_load(zone_ids->snat, MFF_LOG_SNAT_ZONE, 0, 32, ofpacts_p);
        }
    }
}

static void
put_drop(const struct physical_debug *debug, uint8_t table_id,
         struct ofpbuf *ofpacts)
{
    if (debug->collector_set_id) {
        struct ofpact_sample *os = ofpact_put_SAMPLE(ofpacts);
        os->probability = UINT16_MAX;
        os->collector_set_id = debug->collector_set_id;
        os->obs_domain_imm = (debug->obs_domain_id << 24);
        os->obs_point_imm = table_id;
        os->sampling_port = OFPP_NONE;
    }
}

static void
add_default_drop_flow(const struct physical_ctx *p_ctx,
                      uint8_t table_id,
                      struct ovn_desired_flow_table *flow_table)
{
    struct match match = MATCH_CATCHALL_INITIALIZER;
    struct ofpbuf ofpacts;
    ofpbuf_init(&ofpacts, 0);

    put_drop(&p_ctx->debug, table_id, &ofpacts);
    ofctrl_add_flow(flow_table, table_id, 0, 0, &match,
                    &ofpacts, hc_uuid);

    ofpbuf_uninit(&ofpacts);
}

static void
put_local_common_flows(uint32_t dp_key,
                       const struct sbrec_port_binding *pb,
                       const struct sbrec_port_binding *parent_pb,
                       const struct zone_ids *zone_ids,
                       const struct physical_debug *debug,
                       struct ofpbuf *ofpacts_p,
                       struct ovn_desired_flow_table *flow_table)
{
    struct match match;

    uint32_t port_key = pb->tunnel_key;

    /* Table 43, priority 100.
     * =======================
     *
     * Implements output to local hypervisor.  Each flow matches a
     * logical output port on the local hypervisor, and resubmits to
     * table 44.
     */

    ofpbuf_clear(ofpacts_p);

    /* Match MFF_LOG_DATAPATH, MFF_LOG_OUTPORT. */
    match_outport_dp_and_port_keys(&match, dp_key, port_key);

    put_zones_ofpacts(zone_ids, ofpacts_p);

    /* Resubmit to table 44. */
    put_resubmit(OFTABLE_CHECK_LOOPBACK, ofpacts_p);
    ofctrl_add_flow(flow_table, OFTABLE_LOCAL_OUTPUT, 100,
                    pb->header_.uuid.parts[0], &match, ofpacts_p,
                    &pb->header_.uuid);

    /* Table 44, Priority 100.
     * =======================
     *
     * Drop packets whose logical inport and outport are the same
     * and the MLF_ALLOW_LOOPBACK flag is not set. */
    match_init_catchall(&match);
    ofpbuf_clear(ofpacts_p);
    put_drop(debug, OFTABLE_CHECK_LOOPBACK, ofpacts_p);
    match_set_metadata(&match, htonll(dp_key));
    match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                         0, MLF_ALLOW_LOOPBACK);
    match_set_reg(&match, MFF_LOG_INPORT - MFF_REG0, port_key);
    match_set_reg(&match, MFF_LOG_OUTPORT - MFF_REG0, port_key);
    ofctrl_add_flow(flow_table, OFTABLE_CHECK_LOOPBACK, 100,
                    pb->header_.uuid.parts[0], &match, ofpacts_p,
                    &pb->header_.uuid);

    /* Table 64, Priority 100.
     * =======================
     *
     * If the packet is supposed to hair-pin because the
     *   - "loopback" flag is set
     *   - or if the destination is a nested container
     *   - or if "nested_container" flag is set and the destination is the
     *     parent port,
     * temporarily set the in_port to OFPP_NONE, resubmit to
     * table 65 for logical-to-physical translation, then restore
     * the port number.
     *
     * If 'parent_pb' is not NULL, then the 'pb' represents a nested
     * container.
     *
     * Note:We can set in_port to 0 too. But if recirculation happens
     * later (eg. clone action to enter peer pipeline and a subsequent
     * ct action), ovs-vswitchd will drop the packet if the frozen metadata
     * in_port is 0.
     * */

    bool nested_container = parent_pb ? true: false;
    ofpbuf_clear(ofpacts_p);
    match_outport_dp_and_port_keys(&match, dp_key, port_key);
    if (!nested_container) {
        match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                             MLF_ALLOW_LOOPBACK, MLF_ALLOW_LOOPBACK);
    }

    put_stack(MFF_IN_PORT, ofpact_put_STACK_PUSH(ofpacts_p));
    put_load(ofp_to_u16(OFPP_NONE), MFF_IN_PORT, 0, 16, ofpacts_p);
    put_resubmit(OFTABLE_LOG_TO_PHY, ofpacts_p);
    put_stack(MFF_IN_PORT, ofpact_put_STACK_POP(ofpacts_p));
    ofctrl_add_flow(flow_table, OFTABLE_SAVE_INPORT, 100,
                    pb->header_.uuid.parts[0], &match, ofpacts_p,
                    &pb->header_.uuid);

    if (nested_container) {
        /* It's a nested container and when the packet from the nested
         * container is to be sent to the parent port, "nested_container"
         * flag will be set. We need to temporarily set the in_port to
         * OFPP_NONE as mentioned in the comment above.
         *
         * If a parent port has multiple child ports, then this if condition
         * will be hit multiple times, but we want to add only one flow.
         * ofctrl_add_flow() logs a warning message for duplicate flows.
         * So use the function 'ofctrl_check_and_add_flow_metered' which
         * doesn't log a warning.
         *
         * Other option is to add this flow for all the ports which are not
         * nested containers. In which case we will add this flow for all the
         * ports even if they don't have any child ports which is
         * unnecessary.
         */
        ofpbuf_clear(ofpacts_p);
        match_outport_dp_and_port_keys(&match, dp_key, parent_pb->tunnel_key);
        match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                             MLF_NESTED_CONTAINER, MLF_NESTED_CONTAINER);

        put_stack(MFF_IN_PORT, ofpact_put_STACK_PUSH(ofpacts_p));
        put_load(ofp_to_u16(OFPP_NONE), MFF_IN_PORT, 0, 16, ofpacts_p);
        put_resubmit(OFTABLE_LOG_TO_PHY, ofpacts_p);
        put_stack(MFF_IN_PORT, ofpact_put_STACK_POP(ofpacts_p));
        ofctrl_check_and_add_flow_metered(flow_table, OFTABLE_SAVE_INPORT, 100,
                                          parent_pb->header_.uuid.parts[0],
                                          &match, ofpacts_p, &pb->header_.uuid,
                                          NX_CTLR_NO_METER, NULL, false);
    }
}

static size_t
encap_ip_to_id(size_t n_encap_ips, const char **encap_ips,
               const char *ip)
{
    for (size_t i = 0; i < n_encap_ips; i++) {
        if (!strcmp(ip, encap_ips[i])) {
            return i;
        }
    }
    return 0;
}

static void
load_logical_ingress_metadata(const struct sbrec_port_binding *binding,
                              const struct zone_ids *zone_ids,
                              size_t n_encap_ips,
                              const char **encap_ips,
                              struct ofpbuf *ofpacts_p,
                              bool load_encap_id)
{
    put_zones_ofpacts(zone_ids, ofpacts_p);

    /* Set MFF_LOG_DATAPATH and MFF_LOG_INPORT. */
    uint32_t dp_key = binding->datapath->tunnel_key;
    uint32_t port_key = binding->tunnel_key;
    put_load(dp_key, MFF_LOG_DATAPATH, 0, 64, ofpacts_p);
    put_load(port_key, MFF_LOG_INPORT, 0, 32, ofpacts_p);

    if (load_encap_id) {
        /* Default encap_id 0. */
        size_t encap_id = 0;
        if (encap_ips && binding->encap) {
            encap_id = encap_ip_to_id(n_encap_ips, encap_ips,
                                      binding->encap->ip);
        }
        put_load(encap_id, MFF_LOG_ENCAP_ID, 16, 16, ofpacts_p);
    }
}

static bool
pbs_are_peers(const enum en_lport_type type_a,
              const enum en_lport_type type_b)
{
    /* Allow PBs of the same type to be peers. */
    if (type_a == type_b) {
        return true;
    }

    /* Allow L3GW port to be peered with patch port. */
    if ((type_a == LP_L3GATEWAY && type_b == LP_PATCH) ||
        (type_a == LP_PATCH && type_b == LP_L3GATEWAY)) {
        return true;
    }

    return false;
}

static const struct sbrec_port_binding *
get_binding_peer(struct ovsdb_idl_index *sbrec_port_binding_by_name,
                 const struct sbrec_port_binding *binding,
                 const enum en_lport_type type)
{
    const char *peer_name = smap_get(&binding->options, "peer");
    if (!peer_name) {
        return NULL;
    }

    const struct sbrec_port_binding *peer = lport_lookup_by_name(
        sbrec_port_binding_by_name, peer_name);
    if (!peer) {
        return NULL;
    }

    enum en_lport_type peer_type = get_lport_type(peer);
    if (!pbs_are_peers(type, peer_type)) {
        return NULL;
    }

    const char *peer_peer_name = smap_get(&peer->options, "peer");
    if (!peer_peer_name || strcmp(peer_peer_name, binding->logical_port)) {
        return NULL;
    }

    return peer;
}

static bool
physical_should_eval_peer_port(const struct sbrec_port_binding *binding,
                      const struct sbrec_chassis *chassis,
                      const enum en_lport_type type)
{
    return type == LP_PATCH ||
           (type == LP_L3GATEWAY && binding->chassis == chassis);
}

enum access_type {
    PORT_LOCAL = 0,
    PORT_LOCALNET,
    PORT_REMOTE,
    PORT_HA_REMOTE,
};

enum activation_strategy {
    ACTIVATION_RARP = 1 << 0,
    ACTIVATION_GARP = 1 << 1,
    ACTIVATION_NA   = 1 << 2,
    ACTIVATION_IP4  = ACTIVATION_GARP | ACTIVATION_RARP,
    ACTIVATION_IP6  = ACTIVATION_NA,
};

static void
setup_rarp_activation_strategy(const struct sbrec_port_binding *binding,
                               ofp_port_t ofport, struct zone_ids *zone_ids,
                               struct ofpbuf *ofpacts,
                               struct ovn_desired_flow_table *flow_table)
{
    struct match match = MATCH_CATCHALL_INITIALIZER;

    /* Unblock the port on ingress RARP. */
    match_set_dl_type(&match, htons(ETH_TYPE_RARP));
    match_set_in_port(&match, ofport);

    load_logical_ingress_metadata(binding, zone_ids, 0, NULL, ofpacts, true);
    encode_controller_op(ACTION_OPCODE_ACTIVATION_STRATEGY,
                         NX_CTLR_NO_METER, ofpacts);
    put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, ofpacts);

    ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 1010,
                    binding->header_.uuid.parts[0],
                    &match, ofpacts, &binding->header_.uuid);
    ofpbuf_clear(ofpacts);
}

static void
setup_arp_activation_strategy(const struct sbrec_port_binding *binding,
                              ofp_port_t  ofport, struct zone_ids *zone_ids,
                              struct ofpbuf *ofpacts, struct eth_addr mac,
                              ovs_be32 ip,
                              struct ovn_desired_flow_table *flow_table)
{
    struct match match = MATCH_CATCHALL_INITIALIZER;

    /* match: arp/rarp, in_port=<OFPORT>, dl_src=<MAC>, arp_sha=<MAC>,
     * arp_spa=<IP>, arp_tpa=<IP> */
    match_set_dl_type(&match, htons(ETH_TYPE_ARP));
    match_set_in_port(&match, ofport);
    match_set_dl_src(&match, mac);
    match_set_arp_sha(&match, mac);
    match_set_arp_spa_masked(&match, ip, OVS_BE32_MAX);
    match_set_arp_tpa_masked(&match, ip, OVS_BE32_MAX);

    load_logical_ingress_metadata(binding, zone_ids, 0, NULL, ofpacts, true);
    /* Unblock the traffic when it matches specified strategy. */
    encode_controller_op(ACTION_OPCODE_ACTIVATION_STRATEGY,
                         NX_CTLR_NO_METER, ofpacts);
    put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, ofpacts);

    ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 1010,
                    binding->header_.uuid.parts[0],
                    &match, ofpacts, &binding->header_.uuid);
    ofpbuf_clear(ofpacts);
}

static void
setup_nd_na_activation_strategy(const struct sbrec_port_binding *binding,
                                ofp_port_t  ofport, struct zone_ids *zone_ids,
                                struct ofpbuf *ofpacts, struct eth_addr mac,
                                const struct in6_addr *ip,
                                struct ovn_desired_flow_table *flow_table)
{
    struct match match = MATCH_CATCHALL_INITIALIZER;

    /* match: icmp6, in_port=<OFPORT>, icmp_type=136, icmp_code=0,
     * ipv6_src=<IP>, nd_target=<IP>, nd_tll=<MAC> */
    match_set_dl_type(&match, htons(ETH_TYPE_IPV6));
    match_set_in_port(&match, ofport);
    match_set_nw_proto(&match, IPPROTO_ICMPV6);
    match_set_icmp_type(&match, 136);
    match_set_icmp_code(&match, 0);
    match_set_ipv6_src(&match, ip);
    match_set_nd_target(&match, ip);
    match_set_arp_tha(&match, mac);

    load_logical_ingress_metadata(binding, zone_ids, 0, NULL, ofpacts, true);
    /* Unblock the traffic when it matches specified strategy. */
    encode_controller_op(ACTION_OPCODE_ACTIVATION_STRATEGY,
                         NX_CTLR_NO_METER, ofpacts);
    put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, ofpacts);

    ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 1010,
                    binding->header_.uuid.parts[0],
                    &match, ofpacts, &binding->header_.uuid);
    ofpbuf_clear(ofpacts);
}

static void
setup_activation_strategy_flows(const struct sbrec_port_binding *binding,
                                ofp_port_t ofport, struct zone_ids *zone_ids,
                                uint32_t activation_strategies,
                                const struct lport_addresses *addresses,
                                struct ovn_desired_flow_table *flow_table)
{
    uint64_t stub[1024 / 8];
    struct ofpbuf ofpacts = OFPBUF_STUB_INITIALIZER(stub);

    bool ip4_activation = (activation_strategies & ACTIVATION_IP4) != 0;
    bool ip6_activation = (activation_strategies & ACTIVATION_IP6) != 0;

    if (activation_strategies & ACTIVATION_RARP) {
        setup_rarp_activation_strategy(binding, ofport, zone_ids, &ofpacts,
                                       flow_table);
    }

    for (size_t i = 0; ip4_activation && i < addresses->n_ipv4_addrs; i++) {
        const struct ipv4_netaddr *address = &addresses->ipv4_addrs[i];
        if (activation_strategies & ACTIVATION_GARP) {
            setup_arp_activation_strategy(binding, ofport, zone_ids, &ofpacts,
                                          addresses->ea, address->addr,
                                          flow_table);
        }
    }

    /* Always add LLA address activation if there is any IPv6 address. */
    if (ip6_activation && addresses->n_ipv6_addrs) {
        struct in6_addr lla;
        in6_generate_lla(addresses->ea, &lla);

        setup_nd_na_activation_strategy(binding, ofport, zone_ids, &ofpacts,
                                        addresses->ea, &lla, flow_table);
    }

    for (size_t i = 0; ip6_activation && i < addresses->n_ipv6_addrs; i++) {
        const struct ipv6_netaddr *address = &addresses->ipv6_addrs[i];
        if (activation_strategies & ACTIVATION_NA) {
            setup_nd_na_activation_strategy(binding, ofport, zone_ids,
                                            &ofpacts, addresses->ea,
                                            &address->addr, flow_table);
        }
    }

    /* Block all non-RARP traffic for the port, both directions. */
    struct match match = MATCH_CATCHALL_INITIALIZER;
    match_set_in_port(&match, ofport);

    ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 1000,
                    binding->header_.uuid.parts[0],
                    &match, &ofpacts, &binding->header_.uuid);

    match_init_catchall(&match);
    uint32_t dp_key = binding->datapath->tunnel_key;
    uint32_t port_key = binding->tunnel_key;
    match_set_metadata(&match, htonll(dp_key));
    match_set_reg(&match, MFF_LOG_OUTPORT - MFF_REG0, port_key);

    ofctrl_add_flow(flow_table, OFTABLE_LOG_TO_PHY, 1000,
                    binding->header_.uuid.parts[0],
                    &match, &ofpacts, &binding->header_.uuid);

    ofpbuf_uninit(&ofpacts);
}

static uint32_t
pb_parse_activation_strategy(const struct sbrec_port_binding *pb)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
    uint32_t strategies = 0;

    struct sset strategies_cfg = SSET_INITIALIZER(&strategies_cfg);
    sset_from_delimited_string(&strategies_cfg,
                               smap_get_def(&pb->options,
                                           "activation-strategy", ""),
                               ",");
    const char *strategy;
    SSET_FOR_EACH (strategy, &strategies_cfg) {
        if (!strcmp(strategy, "rarp")) {
            strategies |= ACTIVATION_RARP;
        } else if (!strcmp(strategy, "garp")) {
            strategies |= ACTIVATION_GARP;
        } else if (!strcmp(strategy, "na")) {
            strategies |= ACTIVATION_NA;
        } else {
            VLOG_WARN_RL(&rl, "Unknown activation strategy defined for "
                         "port %s: %s", pb->logical_port, strategy);
        }
    }

    sset_destroy(&strategies_cfg);
    return strategies;
}

static void
setup_activation_strategy(const struct sbrec_port_binding *binding,
                          const struct sbrec_chassis *chassis,
                          uint32_t dp_key, uint32_t port_key,
                          ofp_port_t ofport, struct zone_ids *zone_ids,
                          struct ovn_desired_flow_table *flow_table)
{
    if (!is_additional_chassis(binding, chassis)) {
        return;
    }

    if (lport_is_activated_by_activation_strategy(binding, chassis) ||
        pinctrl_is_port_activated(dp_key, port_key)) {
        return;
    }

    uint32_t strategies = pb_parse_activation_strategy(binding);
    if (!strategies) {
        return;
    }

    for (size_t i = 0; i < binding->n_mac; i++) {
        struct lport_addresses addresses;
        if (!extract_lsp_addresses(binding->mac[i], &addresses)) {
            continue;
        }
        setup_activation_strategy_flows(binding, ofport, zone_ids, strategies,
                                        &addresses, flow_table);
        destroy_lport_addresses(&addresses);
    }
}

/*
 * Insert a flow to determine if an IP packet is too big for the corresponding
 * egress interface.
 */
static void
determine_if_pkt_too_big(struct ovn_desired_flow_table *flow_table,
                         const struct sbrec_port_binding *binding,
                         const struct sbrec_port_binding *mcp,
                         uint16_t mtu, bool is_ipv6, int direction)
{
    struct ofpbuf ofpacts;
    ofpbuf_init(&ofpacts, 0);

    /* Store packet too large flag in reg9[1]. */
    struct match match;
    match_init_catchall(&match);
    match_set_dl_type(&match, htons(is_ipv6 ? ETH_TYPE_IPV6 : ETH_TYPE_IP));
    match_set_metadata(&match, htonll(binding->datapath->tunnel_key));
    match_set_reg(&match, direction - MFF_REG0, mcp->tunnel_key);

    /* reg9[1] is REGBIT_PKT_LARGER as defined by northd */
    struct ofpact_check_pkt_larger *pkt_larger =
        ofpact_put_CHECK_PKT_LARGER(&ofpacts);
    pkt_larger->pkt_len = mtu;
    pkt_larger->dst.field = mf_from_id(MFF_REG9);
    pkt_larger->dst.ofs = 1;

    put_resubmit(OFTABLE_OUTPUT_LARGE_PKT_PROCESS, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_OUTPUT_LARGE_PKT_DETECT, 100,
                    binding->header_.uuid.parts[0], &match, &ofpacts,
                    &binding->header_.uuid);
    ofpbuf_uninit(&ofpacts);
}

/*
 * Insert a flow to reply with ICMP error for IP packets that are too big for
 * the corresponding egress interface.
 */
/*
 * NOTE(ihrachys) This reimplements icmp_error as found in
 * build_icmperr_pkt_big_flows. We may look into reusing the existing OVN
 * action for this flow in the future.
 */
static void
reply_imcp_error_if_pkt_too_big(struct ovn_desired_flow_table *flow_table,
                                const struct sbrec_port_binding *binding,
                                const struct sbrec_port_binding *mcp,
                                uint16_t mtu, bool is_ipv6, int direction)
{
    struct match match;
    match_init_catchall(&match);
    match_set_dl_type(&match, htons(is_ipv6 ? ETH_TYPE_IPV6 : ETH_TYPE_IP));
    match_set_metadata(&match, htonll(binding->datapath->tunnel_key));
    match_set_reg(&match, direction - MFF_REG0, mcp->tunnel_key);
    match_set_reg_masked(&match, MFF_REG9 - MFF_REG0, 1 << 1, 1 << 1);

    /* Return ICMP error with a part of the original IP packet included. */
    struct ofpbuf ofpacts;
    ofpbuf_init(&ofpacts, 0);
    size_t oc_offset = encode_start_controller_op(
        ACTION_OPCODE_ICMP, true, NX_CTLR_NO_METER, &ofpacts);

    struct ofpbuf inner_ofpacts;
    ofpbuf_init(&inner_ofpacts, 0);

    /* The error packet is no longer too large, set REGBIT_PKT_LARGER = 0 */
    /* reg9[1] is REGBIT_PKT_LARGER as defined by northd */
    ovs_be32 value = htonl(0);
    ovs_be32 mask = htonl(1 << 1);
    ofpact_put_set_field(
        &inner_ofpacts, mf_from_id(MFF_REG9), &value, &mask);

    /* The new error packet is delivered locally */
    /* REGBIT_EGRESS_LOOPBACK = 1 */
    value = htonl(1 << MLF_ALLOW_LOOPBACK_BIT);
    mask = htonl(1 << MLF_ALLOW_LOOPBACK_BIT);
    ofpact_put_set_field(
        &inner_ofpacts, mf_from_id(MFF_LOG_FLAGS), &value, &mask);

    /* inport <-> outport */
    put_stack(MFF_LOG_INPORT, ofpact_put_STACK_PUSH(&inner_ofpacts));
    put_stack(MFF_LOG_OUTPORT, ofpact_put_STACK_PUSH(&inner_ofpacts));
    put_stack(MFF_LOG_INPORT, ofpact_put_STACK_POP(&inner_ofpacts));
    put_stack(MFF_LOG_OUTPORT, ofpact_put_STACK_POP(&inner_ofpacts));

    /* eth.src <-> eth.dst */
    put_stack(MFF_ETH_DST, ofpact_put_STACK_PUSH(&inner_ofpacts));
    put_stack(MFF_ETH_SRC, ofpact_put_STACK_PUSH(&inner_ofpacts));
    put_stack(MFF_ETH_DST, ofpact_put_STACK_POP(&inner_ofpacts));
    put_stack(MFF_ETH_SRC, ofpact_put_STACK_POP(&inner_ofpacts));

    /* ip.src <-> ip.dst */
    put_stack(is_ipv6 ? MFF_IPV6_DST : MFF_IPV4_DST,
        ofpact_put_STACK_PUSH(&inner_ofpacts));
    put_stack(is_ipv6 ? MFF_IPV6_SRC : MFF_IPV4_SRC,
        ofpact_put_STACK_PUSH(&inner_ofpacts));
    put_stack(is_ipv6 ? MFF_IPV6_DST : MFF_IPV4_DST,
        ofpact_put_STACK_POP(&inner_ofpacts));
    put_stack(is_ipv6 ? MFF_IPV6_SRC : MFF_IPV4_SRC,
        ofpact_put_STACK_POP(&inner_ofpacts));

    /* ip.ttl = 255 */
    struct ofpact_ip_ttl *ip_ttl = ofpact_put_SET_IP_TTL(&inner_ofpacts);
    ip_ttl->ttl = 255;

    uint16_t frag_mtu = mtu - ETHERNET_OVERHEAD;
    size_t note_offset;
    if (is_ipv6) {
        /* icmp6.type = 2 (Packet Too Big) */
        /* icmp6.code = 0 */
        uint8_t icmp_type = 2;
        uint8_t icmp_code = 0;
        ofpact_put_set_field(
            &inner_ofpacts, mf_from_id(MFF_ICMPV6_TYPE), &icmp_type, NULL);
        ofpact_put_set_field(
            &inner_ofpacts, mf_from_id(MFF_ICMPV6_CODE), &icmp_code, NULL);

        /* icmp6.frag_mtu */
        note_offset = encode_start_ovn_field_note(OVN_ICMP6_FRAG_MTU,
                                                  &inner_ofpacts);
        ovs_be32 frag_mtu_ovs = htonl(frag_mtu);
        ofpbuf_put(&inner_ofpacts, &frag_mtu_ovs, sizeof(frag_mtu_ovs));
    } else {
        /* icmp4.type = 3 (Destination Unreachable) */
        /* icmp4.code = 4 (Fragmentation Needed) */
        uint8_t icmp_type = 3;
        uint8_t icmp_code = 4;
        ofpact_put_set_field(
            &inner_ofpacts, mf_from_id(MFF_ICMPV4_TYPE), &icmp_type, NULL);
        ofpact_put_set_field(
            &inner_ofpacts, mf_from_id(MFF_ICMPV4_CODE), &icmp_code, NULL);

        /* icmp4.frag_mtu = */
        note_offset = encode_start_ovn_field_note(OVN_ICMP4_FRAG_MTU,
                                                  &inner_ofpacts);
        ovs_be16 frag_mtu_ovs = htons(frag_mtu);
        ofpbuf_put(&inner_ofpacts, &frag_mtu_ovs, sizeof(frag_mtu_ovs));
    }
    encode_finish_ovn_field_note(note_offset, &inner_ofpacts);

    /* Finally, submit the ICMP error back to the ingress pipeline */
    put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, &inner_ofpacts);

    /* Attach nested actions to ICMP error controller handler */
    ofpacts_put_openflow_actions(inner_ofpacts.data, inner_ofpacts.size,
                                 &ofpacts, OFP15_VERSION);

    /* Finalize the ICMP error controller handler */
    encode_finish_controller_op(oc_offset, &ofpacts);

    ofctrl_add_flow(flow_table, OFTABLE_OUTPUT_LARGE_PKT_PROCESS, 100,
                    binding->header_.uuid.parts[0], &match, &ofpacts,
                    &binding->header_.uuid);

    ofpbuf_uninit(&inner_ofpacts);
    ofpbuf_uninit(&ofpacts);
}

static uint16_t
get_tunnel_overhead(struct chassis_tunnel const *tun)
{
    uint16_t overhead = 0;
    enum chassis_tunnel_type type = tun->type;
    if (type == GENEVE) {
        overhead += GENEVE_TUNNEL_OVERHEAD;
    } else if (type == VXLAN) {
        overhead += VXLAN_TUNNEL_OVERHEAD;
    } else {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "Unknown tunnel type %d, can't determine overhead "
                          "size for Path MTU Discovery", type);
        return 0;
    }
    overhead += tun->is_ipv6? IPV6_HEADER_LEN : IP_HEADER_LEN;
    return overhead;
}

static uint16_t
get_effective_mtu(const struct sbrec_port_binding *mcp,
                  struct vector *remote_tunnels,
                  const struct if_status_mgr *if_mgr)
{
    /* Use interface MTU as a base for calculation */
    uint16_t iface_mtu = if_status_mgr_iface_get_mtu(if_mgr,
                                                     mcp->logical_port);
    if (!iface_mtu) {
        return 0;
    }

    /* Iterate over all peer tunnels and find the biggest tunnel overhead */
    uint16_t overhead = 0;
    const struct chassis_tunnel *tun;
    VECTOR_FOR_EACH (remote_tunnels, tun) {
        overhead = MAX(overhead, get_tunnel_overhead(tun));
    }
    if (!overhead) {
        return 0;
    }

    return iface_mtu - overhead;
}

static void
handle_pkt_too_big_for_ip_version(struct ovn_desired_flow_table *flow_table,
                                  const struct sbrec_port_binding *binding,
                                  const struct sbrec_port_binding *mcp,
                                  uint16_t mtu, bool is_ipv6)
{
    /* ingress */
    determine_if_pkt_too_big(flow_table, binding, mcp, mtu, is_ipv6,
                             MFF_LOG_INPORT);
    reply_imcp_error_if_pkt_too_big(flow_table, binding, mcp, mtu, is_ipv6,
                                    MFF_LOG_INPORT);

    /* egress */
    determine_if_pkt_too_big(flow_table, binding, mcp, mtu, is_ipv6,
                             MFF_LOG_OUTPORT);
    reply_imcp_error_if_pkt_too_big(flow_table, binding, mcp, mtu, is_ipv6,
                                    MFF_LOG_OUTPORT);
}

static void
handle_pkt_too_big(struct ovn_desired_flow_table *flow_table,
                   struct vector *remote_tunnels,
                   const struct sbrec_port_binding *binding,
                   const struct sbrec_port_binding *mcp,
                   const struct if_status_mgr *if_mgr)
{
    uint16_t mtu = get_effective_mtu(mcp, remote_tunnels, if_mgr);
    if (!mtu) {
        return;
    }
    handle_pkt_too_big_for_ip_version(flow_table, binding, mcp, mtu, false);
    handle_pkt_too_big_for_ip_version(flow_table, binding, mcp, mtu, true);
}

static void
enforce_tunneling_for_multichassis_ports(
    struct local_datapath *ld,
    const struct sbrec_port_binding *binding,
    const enum en_lport_type type,
    const struct physical_ctx *ctx,
    struct ovn_desired_flow_table *flow_table)
{
    if (shash_is_empty(&ld->multichassis_ports)) {
        return;
    }

    struct vector tuns = get_remote_tunnels(binding, ctx, NULL);
    if (vector_is_empty(&tuns)) {
        vector_destroy(&tuns);
        return;
    }

    uint32_t dp_key = binding->datapath->tunnel_key;
    uint32_t port_key = binding->tunnel_key;

    struct shash_node *node;
    SHASH_FOR_EACH (node, &ld->multichassis_ports) {
        const struct sbrec_port_binding *mcp = node->data;

        struct ofpbuf ofpacts;
        ofpbuf_init(&ofpacts, 0);

        bool is_vtep_port = type == LP_VTEP;
        /* rewrite MFF_IN_PORT to bypass OpenFlow loopback check for ARP/ND
         * responder in L3 networks. */
        if (is_vtep_port) {
            put_load(ofp_to_u16(OFPP_NONE), MFF_IN_PORT, 0, 16, &ofpacts);
        }

        struct match match;
        match_outport_dp_and_port_keys(&match, dp_key, port_key);
        match_set_reg(&match, MFF_LOG_INPORT - MFF_REG0, mcp->tunnel_key);

        const struct chassis_tunnel *tun;
        VECTOR_FOR_EACH (&tuns, tun) {
            put_encapsulation(ctx->mff_ovn_geneve, tun, binding->datapath,
                              port_key, is_vtep_port, &ofpacts);
            ofpact_put_OUTPUT(&ofpacts)->port = tun->ofport;
        }
        ofctrl_add_flow(flow_table, OFTABLE_REMOTE_OUTPUT, 110,
                        binding->header_.uuid.parts[0], &match, &ofpacts,
                        &binding->header_.uuid);
        ofpbuf_uninit(&ofpacts);

        handle_pkt_too_big(flow_table, &tuns, binding, mcp, ctx->if_mgr);
    }
    vector_destroy(&tuns);
}

static void
consider_port_binding(const struct physical_ctx *ctx,
                      const struct sbrec_port_binding *binding,
                      const enum en_lport_type type,
                      struct ovn_desired_flow_table *flow_table,
                      struct ofpbuf *ofpacts_p)
{
    uint32_t dp_key = binding->datapath->tunnel_key;
    uint32_t port_key = binding->tunnel_key;
    struct local_datapath *ld;
    if (!(ld = get_local_datapath(ctx->local_datapaths, dp_key))) {
        return;
    }

    if (type == LP_VIF) {
        /* Table 80, priority 100.
         * =======================
         *
         * Process ICMP{4,6} error packets too big locally generated from the
         * kernel in order to lookup proper ct_zone. */
        struct match match = MATCH_CATCHALL_INITIALIZER;
        match_set_metadata(&match, htonll(dp_key));
        match_set_reg(&match, MFF_LOG_INPORT - MFF_REG0, port_key);

        struct zone_ids icmp_zone_ids = get_zone_ids(binding, ctx->ct_zones);
        ofpbuf_clear(ofpacts_p);
        put_zones_ofpacts(&icmp_zone_ids, ofpacts_p);
        put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, ofpacts_p);
        ofctrl_add_flow(flow_table, OFTABLE_CT_ZONE_LOOKUP, 100,
                        binding->header_.uuid.parts[0], &match,
                        ofpacts_p, &binding->header_.uuid);
        ofpbuf_clear(ofpacts_p);
    }

    struct match match;
    if (physical_should_eval_peer_port(binding, ctx->chassis, type)) {
        const struct sbrec_port_binding *peer = get_binding_peer(
                ctx->sbrec_port_binding_by_name, binding, type);
        if (!peer) {
            return;
        }

        struct zone_ids binding_zones = get_zone_ids(binding, ctx->ct_zones);
        put_local_common_flows(dp_key, binding, NULL, &binding_zones,
                               &ctx->debug, ofpacts_p, flow_table);

        ofpbuf_clear(ofpacts_p);
        match_outport_dp_and_port_keys(&match, dp_key, port_key);

        size_t clone_ofs = ofpacts_p->size;
        struct ofpact_nest *clone = ofpact_put_CLONE(ofpacts_p);
        ofpact_put_CT_CLEAR(ofpacts_p);
        put_load(0, MFF_LOG_DNAT_ZONE, 0, 32, ofpacts_p);
        put_load(0, MFF_LOG_SNAT_ZONE, 0, 32, ofpacts_p);
        put_load(0, MFF_LOG_CT_ZONE, 0, 16, ofpacts_p);
        struct zone_ids peer_zones = get_zone_ids(peer, ctx->ct_zones);
        load_logical_ingress_metadata(peer, &peer_zones, ctx->n_encap_ips,
                                      ctx->encap_ips, ofpacts_p, false);
        put_load(0, MFF_LOG_FLAGS, 0, 32, ofpacts_p);
        put_load(0, MFF_LOG_OUTPORT, 0, 32, ofpacts_p);
        for (int i = 0; i < MFF_N_LOG_REGS; i++) {
            put_load(0, MFF_LOG_REG0 + i, 0, 32, ofpacts_p);
        }
        put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, ofpacts_p);
        clone = ofpbuf_at_assert(ofpacts_p, clone_ofs, sizeof *clone);
        ofpacts_p->header = clone;
        ofpact_finish_CLONE(ofpacts_p, &clone);

        ofctrl_add_flow(flow_table, OFTABLE_LOG_TO_PHY, 100,
                        binding->header_.uuid.parts[0],
                        &match, ofpacts_p, &binding->header_.uuid);
        return;
    }
    if (type == LP_CHASSISREDIRECT
        && (binding->chassis == ctx->chassis ||
            ha_chassis_group_is_active(binding->ha_chassis_group,
                                       ctx->active_tunnels, ctx->chassis))) {

        /* Table 43, priority 100.
         * =======================
         *
         * Implements output to local hypervisor.  Each flow matches a
         * logical output port on the local hypervisor, and resubmits to
         * table 41.  For ports of type "chassisredirect", the logical
         * output port is changed from the "chassisredirect" port to the
         * underlying distributed port. */

        ofpbuf_clear(ofpacts_p);
        match_outport_dp_and_port_keys(&match, dp_key, port_key);

        const char *distributed_port = smap_get_def(&binding->options,
                                                    "distributed-port", "");
        const struct sbrec_port_binding *distributed_binding
            = lport_lookup_by_name(ctx->sbrec_port_binding_by_name,
                                   distributed_port);

        if (!distributed_binding) {
            /* Packet will be dropped. */
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "No port binding record for distributed "
                         "port %s referred by chassisredirect port %s",
                         distributed_port,
                         binding->logical_port);
        } else if (binding->datapath !=
                   distributed_binding->datapath) {
            /* Packet will be dropped. */
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl,
                         "chassisredirect port %s refers to "
                         "distributed port %s in wrong datapath",
                         binding->logical_port,
                         distributed_port);
        } else {
            put_load(distributed_binding->tunnel_key,
                     MFF_LOG_OUTPORT, 0, 32, ofpacts_p);

            struct zone_ids zone_ids = get_zone_ids(distributed_binding,
                                                    ctx->ct_zones);
            put_zones_ofpacts(&zone_ids, ofpacts_p);

            /* Clear the MFF_INPORT.  Its possible that the same packet may
             * go out from the same tunnel inport. */
            put_load(ofp_to_u16(OFPP_NONE), MFF_IN_PORT, 0, 16, ofpacts_p);

            /* Resubmit to table 41. */
            put_resubmit(OFTABLE_CHECK_LOOPBACK, ofpacts_p);
        }

        ofctrl_add_flow(flow_table, OFTABLE_LOCAL_OUTPUT, 100,
                        binding->header_.uuid.parts[0],
                        &match, ofpacts_p, &binding->header_.uuid);

        return;
    }

    /* Find the OpenFlow port for the logical port, as 'ofport'.  This is
     * one of:
     *
     *     - If the port is a VIF on the chassis we're managing, the
     *       OpenFlow port for the VIF.
     *
     *       The same logic handles ports that OVN implements as Open vSwitch
     *       patch ports, that is, "localnet" and "l2gateway" ports.
     *
     *       For a container nested inside a VM and accessible via a VLAN,
     *       'tag' is the VLAN ID; otherwise 'tag' is 0.
     *
     *       For a localnet or l2gateway patch port, if a VLAN ID was
     *       configured, 'tag' is set to that VLAN ID; otherwise 'tag' is 0.
     */

    int tag = 0;
    bool nested_container = false;
    const struct sbrec_port_binding *binding_port = NULL;
    ofp_port_t ofport;
    bool is_mirror = (type == LP_MIRROR) ? true : false;
    if ((binding->parent_port && *binding->parent_port) || is_mirror) {
        if (!binding->tag && !is_mirror) {
            return;
        }

        const char *binding_port_name = is_mirror ? binding->mirror_port :
                                        binding->parent_port;
        ofport = local_binding_get_lport_ofport(ctx->local_bindings,
                                                binding_port_name);
        if (ofport) {
            if (!is_mirror) {
                tag = *binding->tag;
            }
            nested_container = true;

            binding_port
                = lport_lookup_by_name(ctx->sbrec_port_binding_by_name,
                                       binding_port_name);

            if (binding_port
                && !lport_can_bind_on_this_chassis(ctx->chassis,
                                                  binding_port)) {
                /* Even though there is an ofport for this container
                 * parent port, it is requested on different chassis ignore
                 * this ofport.
                 */
                ofport = 0;
            }
        }
    } else if (type == LP_LOCALNET || type == LP_L2GATEWAY) {

        ofport = u16_to_ofp(simap_get(ctx->patch_ofports,
                                      binding->logical_port));
        if (ofport && binding->tag) {
            tag = *binding->tag;
        }
    } else {
        ofport = local_binding_get_lport_ofport(ctx->local_bindings,
                                                binding->logical_port);
        if (ofport && !lport_can_bind_on_this_chassis(ctx->chassis, binding)) {
            /* Even though there is an ofport for this port_binding, it is
             * requested on different chassis. So ignore this ofport.
             */
            ofport = 0;
        }
    }

    const struct sbrec_port_binding *localnet_port =
        get_localnet_port(ctx->local_datapaths, dp_key);

    struct ha_chassis_ordered *ha_ch_ordered;
    ha_ch_ordered = ha_chassis_get_ordered(binding->ha_chassis_group);

    /* Determine how the port is accessed. */
    enum access_type access_type = PORT_LOCAL;
    if (!ofport) {
        /* Enforce tunneling while we clone packets to additional chassis b/c
         * otherwise upstream switch won't flood the packet to both chassis. */
        if (localnet_port && !binding->additional_chassis) {
            ofport = u16_to_ofp(simap_get(ctx->patch_ofports,
                                          localnet_port->logical_port));
            if (!ofport) {
                goto out;
            }
            access_type = PORT_LOCALNET;
        } else {
            if (!ha_ch_ordered || ha_ch_ordered->n_ha_ch < 2) {
                access_type = PORT_REMOTE;
            } else {
                /* It's distributed across the chassis belonging to
                 * an HA chassis group. */
                access_type = PORT_HA_REMOTE;
            }
        }
    }

    if (access_type == PORT_LOCAL) {
        /* Packets that arrive from a vif can belong to a VM or
         * to a container located inside that VM. Packets that
         * arrive from containers have a tag (vlan) associated with them.
         */

        struct zone_ids zone_ids = get_zone_ids(binding, ctx->ct_zones);
        /* Pass the parent port binding if the port is a nested
         * container. */
        put_local_common_flows(dp_key, binding, binding_port, &zone_ids,
                               &ctx->debug, ofpacts_p, flow_table);

        /* Table 0, Priority 150 and 100.
         * ==============================
         *
         * Priority 150 is for tagged traffic. This may be containers in a
         * VM or a VLAN on a local network. For such traffic, match on the
         * tags and then strip the tag.
         *
         * Priority 100 is for traffic belonging to VMs or untagged locally
         * connected networks.
         *
         * For both types of traffic: set MFF_LOG_INPORT to the logical
         * input port, MFF_LOG_DATAPATH to the logical datapath, and
         * resubmit into the logical ingress pipeline starting at table
         * 16. */
        ofpbuf_clear(ofpacts_p);
        match_init_catchall(&match);
        match_set_in_port(&match, ofport);

        /* Match a VLAN tag and strip it, including stripping priority tags
         * (e.g. VLAN ID 0).  In the latter case we'll add a second flow
         * for frames that lack any 802.1Q header later. */
        if (tag || type == LP_LOCALNET || type == LP_L2GATEWAY) {
            if (nested_container) {
                /* When a packet comes from a container sitting behind a
                 * parent_port, we should let it loopback to other containers
                 * or the parent_port itself. Indicate this by setting the
                 * MLF_NESTED_CONTAINER_BIT in MFF_LOG_FLAGS.*/
                put_load(1, MFF_LOG_FLAGS, MLF_NESTED_CONTAINER_BIT, 1,
                         ofpacts_p);
            }

            /* For vlan-passthru switch ports that are untagged, skip
             * matching/stripping VLAN header that originates from the VIF
             * itself. */
            bool passthru = smap_get_bool(&binding->options,
                                          "vlan-passthru", false);
            if (!passthru || tag) {
                match_set_dl_vlan(&match, htons(tag), 0);
                ofpact_put_STRIP_VLAN(ofpacts_p);
            }
        }

        setup_activation_strategy(binding, ctx->chassis, dp_key, port_key,
                                  ofport, &zone_ids, flow_table);

        /* Remember the size with just strip vlan added so far,
         * as we're going to remove this with ofpbuf_pull() later. */
        uint32_t ofpacts_orig_size = ofpacts_p->size;

        load_logical_ingress_metadata(binding, &zone_ids, ctx->n_encap_ips,
                                      ctx->encap_ips, ofpacts_p, true);

        if (type == LP_LOCALPORT) {
            /* mark the packet as incoming from a localport */
            put_load(1, MFF_LOG_FLAGS, MLF_LOCALPORT_BIT, 1, ofpacts_p);
        }

        /* Resubmit to first logical ingress pipeline table. */
        put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, ofpacts_p);
        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG,
                        tag ? 150 : 100, binding->header_.uuid.parts[0],
                        &match, ofpacts_p, &binding->header_.uuid);

        if (!tag && (type == LP_LOCALNET || type == LP_L2GATEWAY)) {

            /* Add a second flow for frames that lack any 802.1Q
             * header.  For these, drop the OFPACT_STRIP_VLAN
             * action. */
            ofpbuf_pull(ofpacts_p, ofpacts_orig_size);
            match_set_dl_tci_masked(&match, 0, htons(VLAN_CFI));
            ofctrl_add_flow(flow_table, 0, 100,
                            binding->header_.uuid.parts[0], &match, ofpacts_p,
                            &binding->header_.uuid);
        }

        if (type == LP_LOCALNET) {
            put_replace_chassis_mac_flows(ctx->ct_zones, binding,
                                          ctx->local_datapaths, ofpacts_p,
                                          ofport, flow_table);
        }

        /* Table 65, Priority 100.
         * =======================
         *
         * Deliver the packet to the local vif. */
        ofpbuf_clear(ofpacts_p);
        match_outport_dp_and_port_keys(&match, dp_key, port_key);
        if (tag) {
            /* For containers sitting behind a local vif, tag the packets
             * before delivering them. */
            ofpact_put_push_vlan(
                ofpacts_p, localnet_port ? &localnet_port->options : NULL,
                tag);
        }
        ofpact_put_OUTPUT(ofpacts_p)->port = ofport;
        if (tag) {
            /* Revert the tag added to the packets headed to containers
             * in the previous step. If we don't do this, the packets
             * that are to be broadcasted to a VM in the same logical
             * switch will also contain the tag. */
            ofpact_put_STRIP_VLAN(ofpacts_p);
        }
        ofctrl_add_flow(flow_table, OFTABLE_LOG_TO_PHY, 100,
                        binding->header_.uuid.parts[0],
                        &match, ofpacts_p, &binding->header_.uuid);

        if (type == LP_LOCALNET) {
            put_replace_router_port_mac_flows(ctx, binding, ofpacts_p,
                                              ofport, flow_table);
        }

        /* Table 41, priority 160.
         * =======================
         *
         * Do not forward local traffic from a localport to a localnet port.
         */
        if (type == LP_LOCALNET) {
            /* do not forward traffic from localport to localnet port */
            ofpbuf_clear(ofpacts_p);
            put_drop(&ctx->debug, OFTABLE_CHECK_LOOPBACK, ofpacts_p);
            match_outport_dp_and_port_keys(&match, dp_key, port_key);
            match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                                 MLF_LOCALPORT, MLF_LOCALPORT);
            ofctrl_add_flow(flow_table, OFTABLE_CHECK_LOOPBACK, 160,
                            binding->header_.uuid.parts[0], &match,
                            ofpacts_p, &binding->header_.uuid);

            /* Drop LOCAL_ONLY traffic leaking through localnet ports. */
            ofpbuf_clear(ofpacts_p);
            put_drop(&ctx->debug, OFTABLE_CHECK_LOOPBACK, ofpacts_p);
            match_outport_dp_and_port_keys(&match, dp_key, port_key);
            match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                                 MLF_LOCAL_ONLY,
                                 MLF_LOCAL_ONLY | MLF_OVERRIDE_LOCAL_ONLY);
            ofctrl_add_flow(flow_table, OFTABLE_CHECK_LOOPBACK, 160,
                            binding->header_.uuid.parts[0], &match,
                            ofpacts_p, &binding->header_.uuid);

            /* localport traffic directed to external is *not* local */
            struct shash_node *node;
            SHASH_FOR_EACH (node, &ld->external_ports) {
                const struct sbrec_port_binding *pb = node->data;

                /* skip ports that are not claimed by this chassis */
                if (!pb->chassis) {
                    continue;
                }
                if (strcmp(pb->chassis->name, ctx->chassis->name)) {
                    continue;
                }

                ofpbuf_clear(ofpacts_p);
                for (int i = 0; i < MFF_N_LOG_REGS; i++) {
                    put_load(0, MFF_REG0 + i, 0, 32, ofpacts_p);
                }
                put_resubmit(OFTABLE_LOG_EGRESS_PIPELINE, ofpacts_p);

                /* allow traffic directed to external MAC address */
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                for (int i = 0; i < pb->n_mac; i++) {
                    char *err_str;
                    struct eth_addr peer_mac;
                    if ((err_str = str_to_mac(pb->mac[i], &peer_mac))) {
                        VLOG_WARN_RL(
                            &rl, "Parsing MAC failed for external port: %s, "
                                 "with error: %s", pb->logical_port, err_str);
                        free(err_str);
                        continue;
                    }

                    match_outport_dp_and_port_keys(&match, dp_key, port_key);
                    match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                                         MLF_LOCALPORT, MLF_LOCALPORT);
                    match_set_dl_dst(&match, peer_mac);

                    ofctrl_add_flow(flow_table, OFTABLE_CHECK_LOOPBACK, 170,
                                    binding->header_.uuid.parts[0], &match,
                                    ofpacts_p, &binding->header_.uuid);
                }
            }
        }

        /* Table 42, priority 150.
         * =======================
         *
         * Handles packets received from ports of type "localport".  These
         * ports are present on every hypervisor.  Traffic that originates at
         * one should never go over a tunnel to a remote hypervisor,
         * so resubmit them to table 40 for local delivery. */
        if (type == LP_LOCALPORT) {
            ofpbuf_clear(ofpacts_p);
            put_resubmit(OFTABLE_LOCAL_OUTPUT, ofpacts_p);
            match_init_catchall(&match);
            match_set_reg(&match, MFF_LOG_INPORT - MFF_REG0,
                          binding->tunnel_key);
            match_set_metadata(&match, htonll(binding->datapath->tunnel_key));
            ofctrl_add_flow(flow_table, OFTABLE_REMOTE_OUTPUT, 150,
                            binding->header_.uuid.parts[0], &match,
                            ofpacts_p, &binding->header_.uuid);
        }
    } else if (access_type == PORT_LOCALNET && !ctx->always_tunnel) {
        /* Remote port connected by localnet port */
        /* Table 43, priority 100.
         * =======================
         *
         * Implements switching to localnet port. Each flow matches a
         * logical output port on remote hypervisor, switch the output port
         * to connected localnet port and resubmits to same table.
         *
         * Note: If 'always_tunnel' is true, then
         * put_remote_port_redirect_overlay() called from below takes care
         * of adding the flow in OFTABLE_REMOTE_OUTPUT table to tunnel to
         * the destination chassis.
         */

        ofpbuf_clear(ofpacts_p);

        /* Match MFF_LOG_DATAPATH, MFF_LOG_OUTPORT. */
        match_outport_dp_and_port_keys(&match, dp_key, port_key);

        put_load(localnet_port->tunnel_key, MFF_LOG_OUTPORT, 0, 32, ofpacts_p);

        /* Resubmit to table 43. */
        put_resubmit(OFTABLE_LOCAL_OUTPUT, ofpacts_p);
        ofctrl_add_flow(flow_table, OFTABLE_LOCAL_OUTPUT, 100,
                        binding->header_.uuid.parts[0],
                        &match, ofpacts_p, &binding->header_.uuid);

        enforce_tunneling_for_multichassis_ports(ld, binding, type,
                                                 ctx, flow_table);

        /* No more tunneling to set up. */
        goto out;
    }

    /* Send packets to additional chassis if needed. */
    const char *redirect_type = smap_get(&binding->options,
                                         "redirect-type");

    /* Table 43, priority 100.
     * =======================
     *
     * Handles traffic that needs to be sent to a remote hypervisor.  Each
     * flow matches an output port that includes a logical port on a remote
     * hypervisor, and tunnels the packet to that hypervisor.
     */
    ofpbuf_clear(ofpacts_p);
    match_outport_dp_and_port_keys(&match, dp_key, port_key);

    if (redirect_type && !strcasecmp(redirect_type, "bridged")) {
        put_remote_port_redirect_bridged(
            binding, type, ctx->local_datapaths, ld,
            &match, ofpacts_p, flow_table);
    } else if (access_type == PORT_HA_REMOTE) {
        put_remote_port_redirect_overlay_ha_remote(
            binding, type, ha_ch_ordered, ctx->mff_ovn_geneve, port_key,
            &match, ofpacts_p, ctx->chassis_tunnels, flow_table);
    } else {
        put_remote_port_redirect_overlay(
            binding, type, ctx, port_key, &match, ofpacts_p, flow_table);
    }
out:
    if (ha_ch_ordered) {
        ha_chassis_destroy_ordered(ha_ch_ordered);
    }
}

static int64_t
get_vxlan_port_key(int64_t port_key)
{
    if (port_key >= OVN_MIN_MULTICAST) {
        /* 0b1<11 least significant bits> */
        return OVN_VXLAN_MIN_MULTICAST |
            (port_key & (OVN_VXLAN_MIN_MULTICAST - 1));
    }
    return port_key;
}

/* Encapsulate and send to a single remote chassis. */
static void
tunnel_to_chassis(enum mf_field_id mff_ovn_geneve,
                  const char *chassis_name,
                  const struct hmap *chassis_tunnels,
                  const struct sbrec_datapath_binding *datapath,
                  uint16_t outport, struct ofpbuf *remote_ofpacts)
{
    const struct chassis_tunnel *tun
        = chassis_tunnel_find(chassis_tunnels, chassis_name, NULL, NULL);
    if (!tun) {
        return;
    }

    put_encapsulation(mff_ovn_geneve, tun, datapath, outport, false,
                      remote_ofpacts);
    ofpact_put_OUTPUT(remote_ofpacts)->port = tun->ofport;
}

/* Encapsulate and send to a set of remote chassis. */
static void
fanout_to_chassis(enum mf_field_id mff_ovn_geneve,
                  struct sset *remote_chassis,
                  const struct hmap *chassis_tunnels,
                  const struct sbrec_datapath_binding *datapath,
                  uint16_t outport, bool is_ramp_switch,
                  struct ofpbuf *remote_ofpacts)
{
    const char *chassis_name;
    const struct chassis_tunnel *prev = NULL;
    SSET_FOR_EACH (chassis_name, remote_chassis) {
        const struct chassis_tunnel *tun
            = chassis_tunnel_find(chassis_tunnels, chassis_name, NULL, NULL);
        if (!tun) {
            continue;
        }

        if (!prev || tun->type != prev->type) {
            put_encapsulation(mff_ovn_geneve, tun, datapath,
                              outport, is_ramp_switch, remote_ofpacts);
            prev = tun;
        }
        ofpact_put_OUTPUT(remote_ofpacts)->port = tun->ofport;
    }
}

static bool
chassis_is_vtep(const struct sbrec_chassis *chassis)
{
    return smap_get_bool(&chassis->other_config, "is-vtep", false);
}

static void
local_output_pb(int64_t tunnel_key, struct ofpbuf *ofpacts)
{
    put_load(tunnel_key, MFF_LOG_OUTPORT, 0, 32, ofpacts);
    put_resubmit(OFTABLE_CHECK_LOOPBACK, ofpacts);
}

static void
local_set_ct_zone_and_output_pb(int tunnel_key, int64_t zone_id,
                                struct ofpbuf *ofpacts)
{
    if (zone_id) {
        put_load(zone_id, MFF_LOG_CT_ZONE, 0, 16, ofpacts);
    }
    put_load(tunnel_key, MFF_LOG_OUTPORT, 0, 32, ofpacts);
    put_resubmit(OFTABLE_CHECK_LOOPBACK, ofpacts);
}

#define MC_OFPACTS_MAX_MSG_SIZE     8192
#define MC_BUF_START_ID             0x9000

struct mc_buf_split_ctx {
    uint8_t index;
    uint8_t stage;
    uint16_t prio;
    uint32_t flags;
    uint32_t flags_mask;
    struct ofpbuf ofpacts;
};

enum mc_buf_split_type {
    MC_BUF_SPLIT_LOCAL,
    MC_BUF_SPLIT_REMOTE,
    MC_BUF_SPLIT_REMOTE_RAMP,
    MC_BUF_SPLIT_MAX,
};

static void
mc_ofctrl_add_flow(const struct sbrec_multicast_group *mc,
                   struct mc_buf_split_ctx *ctx, bool split,
                   struct ovn_desired_flow_table *flow_table)

{
    struct match match = MATCH_CATCHALL_INITIALIZER;

    match_outport_dp_and_port_keys(&match, mc->datapath->tunnel_key,
                                   mc->tunnel_key);
    match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                         ctx->flags, ctx->flags_mask);

    uint16_t prio = ctx->prio;
    if (ctx->index) {
        match_set_reg(&match, MFF_REG6 - MFF_REG0,
                      MC_BUF_START_ID + ctx->index);
        prio += 10;
    }

    if (split) {
        put_split_buf_function(MC_BUF_START_ID + ctx->index + 1,
                               mc->tunnel_key, ctx->stage, &ctx->ofpacts);
    }

    ofctrl_add_flow(flow_table, ctx->stage, prio, mc->header_.uuid.parts[0],
                    &match, &ctx->ofpacts, &mc->header_.uuid);
    ofpbuf_clear(&ctx->ofpacts);
    /* reset MFF_REG6. */
    put_load(0, MFF_REG6, 0, 32, &ctx->ofpacts);
}

static void
consider_mc_group(const struct physical_ctx *ctx,
                  const struct sbrec_multicast_group *mc,
                  struct ovn_desired_flow_table *flow_table)
{
    struct local_datapath *ldp = get_local_datapath(ctx->local_datapaths,
                                                    mc->datapath->tunnel_key);
    if (!ldp) {
        return;
    }

    struct sset remote_chassis = SSET_INITIALIZER(&remote_chassis);
    struct sset vtep_chassis = SSET_INITIALIZER(&vtep_chassis);

    /* Go through all of the ports in the multicast group:
     *
     *    - For remote ports, add the chassis to 'remote_chassis' or
     *      'vtep_chassis'.
     *
     *    - For local ports (other than logical patch ports), add actions
     *      to 'ofpacts' to set the output port and resubmit.
     *
     *    - For logical patch ports, add actions to 'remote_ofpacts'
     *      instead.  (If we put them in 'ofpacts', then the output
     *      would happen on every hypervisor in the multicast group,
     *      effectively duplicating the packet.)  The only exception
     *      is in case of transit switches (used by OVN-IC).  We need
     *      patch ports to be processed on the remote side, after
     *      crossing the AZ boundary.
     *
     *    - For chassisredirect ports, add actions to 'ofpacts' to
     *      set the output port to be the router patch port for which
     *      the redirect port was added.
     */
    bool has_vtep = has_vtep_port(ctx->local_datapaths,
                                  mc->datapath->tunnel_key);
    struct mc_buf_split_ctx mc_split[MC_BUF_SPLIT_MAX] = {
        {
            .stage = OFTABLE_LOCAL_OUTPUT,
            .prio = 100,
        },
        {
            .stage = OFTABLE_REMOTE_OUTPUT,
            .prio = 100,
            .flags_mask = has_vtep ? MLF_RCV_FROM_RAMP : 0,
        },
        {
            .stage = OFTABLE_REMOTE_OUTPUT,
            .prio = 120,
            .flags = MLF_RCV_FROM_RAMP | MLF_ALLOW_LOOPBACK,
            .flags_mask = MLF_RCV_FROM_RAMP | MLF_ALLOW_LOOPBACK,
        },
    };
    for (size_t i = 0; i < MC_BUF_SPLIT_MAX; i++) {
        struct mc_buf_split_ctx *split_ctx = &mc_split[i];
        ofpbuf_init(&split_ctx->ofpacts, 0);
        put_load(0, MFF_REG6, 0, 32, &split_ctx->ofpacts);
    }

    struct mc_buf_split_ctx *local_ctx = &mc_split[MC_BUF_SPLIT_LOCAL];
    struct mc_buf_split_ctx *remote_ctx = &mc_split[MC_BUF_SPLIT_REMOTE];
    struct mc_buf_split_ctx *ramp_ctx = &mc_split[MC_BUF_SPLIT_REMOTE_RAMP];

    for (size_t i = 0; i < mc->n_ports; i++) {
        struct sbrec_port_binding *port = mc->ports[i];
        enum en_lport_type type = get_lport_type(port);

        if (port->datapath != mc->datapath) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
            VLOG_WARN_RL(&rl, UUID_FMT": multicast group contains ports "
                         "in wrong datapath",
                         UUID_ARGS(&mc->header_.uuid));
            continue;
        }

        int zone_id = ct_zone_find_zone(ctx->ct_zones, port->logical_port);

        const char *lport_name = (port->parent_port && *port->parent_port) ?
                                  port->parent_port : port->logical_port;

        if (type == LP_PATCH) {
            if (ldp->is_transit_switch) {
                local_set_ct_zone_and_output_pb(port->tunnel_key, zone_id,
                                                &local_ctx->ofpacts);
            } else {
                local_output_pb(port->tunnel_key, &remote_ctx->ofpacts);
                local_output_pb(port->tunnel_key, &ramp_ctx->ofpacts);
            }
        } else if (type == LP_REMOTE) {
            if (port->chassis) {
                put_load(port->tunnel_key, MFF_LOG_OUTPORT, 0, 32,
                         &remote_ctx->ofpacts);
                tunnel_to_chassis(ctx->mff_ovn_geneve, port->chassis->name,
                                  ctx->chassis_tunnels, mc->datapath,
                                  port->tunnel_key, &remote_ctx->ofpacts);
            }
        } else if (type == LP_LOCALPORT) {
            local_output_pb(port->tunnel_key, &remote_ctx->ofpacts);
        } else if ((port->chassis == ctx->chassis
                    || is_additional_chassis(port, ctx->chassis))
                   && (local_binding_get_primary_pb(ctx->local_bindings,
                                                    lport_name)
                       || type == LP_L3GATEWAY)) {
            local_set_ct_zone_and_output_pb(port->tunnel_key, zone_id,
                                            &local_ctx->ofpacts);
        } else if (simap_contains(ctx->patch_ofports, port->logical_port)) {
            local_set_ct_zone_and_output_pb(port->tunnel_key, zone_id,
                                            &local_ctx->ofpacts);
        } else if (type == LP_CHASSISREDIRECT
                   && port->chassis == ctx->chassis) {
            const char *distributed_port = smap_get(&port->options,
                                                    "distributed-port");
            if (distributed_port) {
                const struct sbrec_port_binding *distributed_binding
                    = lport_lookup_by_name(ctx->sbrec_port_binding_by_name,
                                           distributed_port);
                if (distributed_binding
                    && port->datapath == distributed_binding->datapath) {
                    local_set_ct_zone_and_output_pb(
                        distributed_binding->tunnel_key, zone_id,
                        &local_ctx->ofpacts);
                }
            }
        } else if (!get_localnet_port(ctx->local_datapaths,
                                      mc->datapath->tunnel_key)) {
            /* Add remote chassis only when localnet port not exist,
             * otherwise multicast will reach remote ports through localnet
             * port. */
            if (port->chassis) {
                if (chassis_is_vtep(port->chassis)) {
                    sset_add(&vtep_chassis, port->chassis->name);
                } else {
                    sset_add(&remote_chassis, port->chassis->name);
                }
            }
            for (size_t j = 0; j < port->n_additional_chassis; j++) {
                if (chassis_is_vtep(port->additional_chassis[j])) {
                    sset_add(&vtep_chassis,
                             port->additional_chassis[j]->name);
                } else {
                    sset_add(&remote_chassis,
                             port->additional_chassis[j]->name);
                }
            }
        }

        for (size_t n = 0; n < MC_BUF_SPLIT_MAX; n++) {
            struct mc_buf_split_ctx *split_ctx = &mc_split[n];
            if (!has_vtep && n == MC_BUF_SPLIT_REMOTE_RAMP) {
                continue;
            }
            if (split_ctx->ofpacts.size >= MC_OFPACTS_MAX_MSG_SIZE) {
                mc_ofctrl_add_flow(mc, split_ctx, true, flow_table);
                split_ctx->index++;
            }
        }
    }

    bool local_lports = local_ctx->ofpacts.size > 0;
    bool remote_ports = remote_ctx->ofpacts.size > 0;
    bool ramp_ports = ramp_ctx->ofpacts.size > 0;

    if (local_lports) {
        put_load(mc->tunnel_key, MFF_LOG_OUTPORT, 0, 32, &local_ctx->ofpacts);
        mc_ofctrl_add_flow(mc, local_ctx, false, flow_table);
    }

    if (remote_ports) {
        put_load(mc->tunnel_key, MFF_LOG_OUTPORT, 0, 32, &remote_ctx->ofpacts);
    }
    fanout_to_chassis(ctx->mff_ovn_geneve, &remote_chassis,
                      ctx->chassis_tunnels, mc->datapath, mc->tunnel_key,
                      false, &remote_ctx->ofpacts);
    fanout_to_chassis(ctx->mff_ovn_geneve, &vtep_chassis,
                      ctx->chassis_tunnels, mc->datapath, mc->tunnel_key,
                      true, &remote_ctx->ofpacts);

    remote_ports = remote_ctx->ofpacts.size > 0;
    if (remote_ports) {
        if (local_lports) {
            put_resubmit(OFTABLE_LOCAL_OUTPUT, &remote_ctx->ofpacts);
        }
        mc_ofctrl_add_flow(mc, remote_ctx, false, flow_table);
    }

    if (ramp_ports && has_vtep) {
        put_load(mc->tunnel_key, MFF_LOG_OUTPORT, 0, 32, &ramp_ctx->ofpacts);
        put_resubmit(OFTABLE_LOCAL_OUTPUT, &ramp_ctx->ofpacts);
        mc_ofctrl_add_flow(mc, ramp_ctx, false, flow_table);
    }

    for (size_t i = 0; i < MC_BUF_SPLIT_MAX; i++) {
        ofpbuf_uninit(&mc_split[i].ofpacts);
    }
    sset_destroy(&remote_chassis);
    sset_destroy(&vtep_chassis);
}

#define CHASSIS_FLOOD_MAX_MSG_SIZE MC_OFPACTS_MAX_MSG_SIZE

static void
physical_eval_remote_chassis_flows(const struct physical_ctx *ctx,
                                   struct ofpbuf *egress_ofpacts,
                                   struct ovn_desired_flow_table *flow_table)
{
    struct match match = MATCH_CATCHALL_INITIALIZER;
    uint32_t index = CHASSIS_FLOOD_INDEX_START;
    struct chassis_tunnel *prev = NULL;

    uint8_t actions_stub[256];
    struct ofpbuf ingress_ofpacts;
    ofpbuf_use_stub(&ingress_ofpacts, actions_stub, sizeof(actions_stub));

    ofpbuf_clear(egress_ofpacts);

    const struct sbrec_chassis *chassis;
    SBREC_CHASSIS_TABLE_FOR_EACH (chassis, ctx->chassis_table) {
        if (!smap_get_bool(&chassis->other_config, "is-remote", false)) {
            continue;
        }

        struct chassis_tunnel *tun =
            chassis_tunnel_find(ctx->chassis_tunnels, chassis->name,
                                NULL, NULL);
        if (!tun) {
            continue;
        }

        /* Do not create flows for Geneve if the TLV negotiation is not
         * finished.
         */
        if (tun->type == GENEVE && !ctx->mff_ovn_geneve) {
            continue;
        }

        if (!(prev && prev->type == tun->type)) {
            put_remote_chassis_flood_encap(egress_ofpacts, tun->type,
                                           ctx->mff_ovn_geneve);
        }

        ofpact_put_OUTPUT(egress_ofpacts)->port = tun->ofport;
        prev = tun;

        if (egress_ofpacts->size > CHASSIS_FLOOD_MAX_MSG_SIZE) {
            match_set_chassis_flood_remote(&match, index++);
            put_split_buf_function(index, 0, OFTABLE_FLOOD_REMOTE_CHASSIS,
                                   egress_ofpacts);

            ofctrl_add_flow(flow_table, OFTABLE_FLOOD_REMOTE_CHASSIS, 100, 0,
                            &match, egress_ofpacts, hc_uuid);

            ofpbuf_clear(egress_ofpacts);
            prev = NULL;
        }


        ofpbuf_clear(&ingress_ofpacts);
        put_load(1, MFF_LOG_FLAGS, MLF_RX_FROM_TUNNEL_BIT, 1,
                 &ingress_ofpacts);
        put_decapsulation(ctx->mff_ovn_geneve, tun, &ingress_ofpacts);
        put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, &ingress_ofpacts);
        if (tun->type == VXLAN) {
            /* VXLAN doesn't carry the inport information, we cannot set
             * the outport to 0 then and match on it. */
            put_resubmit(OFTABLE_LOCAL_OUTPUT, &ingress_ofpacts);
        }

        /* Add match on ARP response coming from remote chassis. */
        match_init_catchall(&match);
        match_set_in_port(&match, tun->ofport);
        match_set_dl_type(&match, htons(ETH_TYPE_ARP));
        match_set_arp_opcode_masked(&match, 2, UINT8_MAX);
        match_set_chassis_flood_outport(&match, tun->type,
                                        ctx->mff_ovn_geneve);

        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 120,
                        chassis->header_.uuid.parts[0],
                        &match, &ingress_ofpacts, hc_uuid);

        /* Add match on ND NA coming from remote chassis. */
        match_init_catchall(&match);
        match_set_in_port(&match, tun->ofport);
        match_set_dl_type(&match, htons(ETH_TYPE_IPV6));
        match_set_nw_proto(&match, IPPROTO_ICMPV6);
        match_set_icmp_type(&match, 136);
        match_set_icmp_code(&match, 0);
        match_set_chassis_flood_outport(&match, tun->type,
                                        ctx->mff_ovn_geneve);

        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 120,
                        chassis->header_.uuid.parts[0],
                        &match, &ingress_ofpacts, hc_uuid);
    }

    if (egress_ofpacts->size > 0) {
        match_set_chassis_flood_remote(&match, index++);

        ofctrl_add_flow(flow_table, OFTABLE_FLOOD_REMOTE_CHASSIS, 100, 0,
                        &match, egress_ofpacts, hc_uuid);
    }

    ofpbuf_uninit(&ingress_ofpacts);
}

static void
physical_eval_port_binding(struct physical_ctx *p_ctx,
                           const struct sbrec_port_binding *pb,
                           const enum en_lport_type type,
                           struct ovn_desired_flow_table *flow_table)
{
    struct ofpbuf ofpacts;
    ofpbuf_init(&ofpacts, 0);
    consider_port_binding(p_ctx, pb, type, flow_table, &ofpacts);
    ofpbuf_uninit(&ofpacts);
}

bool
physical_handle_flows_for_lport(const struct sbrec_port_binding *pb,
                                bool removed, struct physical_ctx *p_ctx,
                                struct ovn_desired_flow_table *flow_table)
{
    enum en_lport_type type = get_lport_type(pb);
    if (type == LP_VTEP) {
        /* Cannot handle changes to vtep lports (yet). */
        return false;
    }

    ofctrl_remove_flows(flow_table, &pb->header_.uuid);

    struct local_datapath *ldp =
        get_local_datapath(p_ctx->local_datapaths,
                           pb->datapath->tunnel_key);

    if (type == LP_EXTERNAL || type == LP_PATCH || type == LP_L3GATEWAY) {
        /* Those lports have a dependency on the localnet port.
         * We need to remove the flows of the localnet port as well
         * and re-consider adding the flows for it.
         */
        if (ldp && ldp->localnet_port) {
            ofctrl_remove_flows(flow_table, &ldp->localnet_port->header_.uuid);
            physical_eval_port_binding(p_ctx, ldp->localnet_port,
                                       get_lport_type(ldp->localnet_port),
                                       flow_table);
        }
    }

    if (sbrec_port_binding_is_updated(
            pb, SBREC_PORT_BINDING_COL_ADDITIONAL_CHASSIS) || removed) {
        physical_multichassis_reprocess(pb, p_ctx, flow_table);
    }

    /* Always update pb and the configured peer for patch ports. */
    if (!removed) {
        physical_eval_port_binding(p_ctx, pb, type, flow_table);
    }

    const struct sbrec_port_binding *peer =
        physical_should_eval_peer_port(pb, p_ctx->chassis, type)
        ? get_binding_peer(p_ctx->sbrec_port_binding_by_name, pb, type)
        : NULL;
    if (peer) {
        ofctrl_remove_flows(flow_table, &peer->header_.uuid);
        physical_eval_port_binding(p_ctx, peer, get_lport_type(peer),
                                   flow_table);
    }

    return true;
}

void
physical_multichassis_reprocess(const struct sbrec_port_binding *pb,
                                struct physical_ctx *p_ctx,
                                struct ovn_desired_flow_table *flow_table)
{
    struct sbrec_port_binding *target =
            sbrec_port_binding_index_init_row(
                    p_ctx->sbrec_port_binding_by_datapath);
    sbrec_port_binding_index_set_datapath(target, pb->datapath);

    const struct sbrec_port_binding *port;
    SBREC_PORT_BINDING_FOR_EACH_EQUAL (port, target,
                                       p_ctx->sbrec_port_binding_by_datapath) {
        /* Ignore PBs that were already reprocessed. */
        if (!sset_add(&p_ctx->reprocessed_pbs, port->logical_port)) {
            continue;
        }

        ofctrl_remove_flows(flow_table, &port->header_.uuid);
        physical_eval_port_binding(p_ctx, port, get_lport_type(port),
                                   flow_table);
    }
    sbrec_port_binding_index_destroy_row(target);
}

void
physical_handle_mc_group_changes(struct physical_ctx *p_ctx,
                                 struct ovn_desired_flow_table *flow_table)
{
    const struct sbrec_multicast_group *mc;
    SBREC_MULTICAST_GROUP_TABLE_FOR_EACH_TRACKED (mc, p_ctx->mc_group_table) {
        if (sbrec_multicast_group_is_deleted(mc)) {
            ofctrl_remove_flows(flow_table, &mc->header_.uuid);
        } else {
            if (!sbrec_multicast_group_is_new(mc)) {
                ofctrl_remove_flows(flow_table, &mc->header_.uuid);
            }
            consider_mc_group(p_ctx, mc, flow_table);
        }
    }
}

void
physical_run(struct physical_ctx *p_ctx,
             struct ovn_desired_flow_table *flow_table)
{
    COVERAGE_INC(physical_run);

    if (!hc_uuid) {
        hc_uuid = xmalloc(sizeof(struct uuid));
        uuid_generate(hc_uuid);
    }

    struct ofpbuf ofpacts;
    ofpbuf_init(&ofpacts, 0);

    put_chassis_mac_conj_id_flow(p_ctx->chassis_table, p_ctx->chassis,
                                 &ofpacts, flow_table);

    /* Set up flows in table 0 for physical-to-logical translation and in table
     * 64 for logical-to-physical translation. */
    const struct sbrec_port_binding *binding;
    SBREC_PORT_BINDING_TABLE_FOR_EACH (binding, p_ctx->port_binding_table) {
        consider_port_binding(p_ctx, binding, get_lport_type(binding),
                              flow_table, &ofpacts);
    }

    /* Default flow for CT_ZONE_LOOKUP Table. */
    struct match ct_look_def_match;
    match_init_catchall(&ct_look_def_match);
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_ZONE_LOOKUP, 0, 0,
                    &ct_look_def_match, &ofpacts, hc_uuid);

    /* Handle output to multicast groups, in tables 40 and 41. */
    const struct sbrec_multicast_group *mc;
    SBREC_MULTICAST_GROUP_TABLE_FOR_EACH (mc, p_ctx->mc_group_table) {
        consider_mc_group(p_ctx, mc, flow_table);
    }

    /* Table 0, priority 100.
     * ======================
     *
     * Process packets that arrive from a remote hypervisor (by matching
     * on tunnel in_port). */

    /* Add flows for Geneve and VXLAN encapsulations.  Geneve encapsulations
     * have metadata about the ingress and egress logical ports.
     * VXLAN encapsulations have metadata about the egress logical port only.
     * We set MFF_LOG_DATAPATH, MFF_LOG_INPORT, and MFF_LOG_OUTPORT from the
     * tunnel key data where possible, then resubmit to table 40 to handle
     * packets to the local hypervisor. */
    struct chassis_tunnel *tun;
    HMAP_FOR_EACH (tun, hmap_node, p_ctx->chassis_tunnels) {
        struct match match = MATCH_CATCHALL_INITIALIZER;
        match_set_in_port(&match, tun->ofport);

        ofpbuf_clear(&ofpacts);
        put_decapsulation(p_ctx->mff_ovn_geneve, tun, &ofpacts);

        put_resubmit(OFTABLE_LOCAL_OUTPUT, &ofpacts);
        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 100, 0, &match,
                        &ofpacts, hc_uuid);

        /* Set allow rx from tunnel bit. */
        put_load(1, MFF_LOG_FLAGS, MLF_RX_FROM_TUNNEL_BIT, 1, &ofpacts);

        /* Add specif flows for E/W ICMPv{4,6} packets if tunnelled packets
         * do not fit path MTU.
         */
        put_resubmit(OFTABLE_CT_ZONE_LOOKUP, &ofpacts);

        /* IPv4 */
        match_init_catchall(&match);
        match_set_in_port(&match, tun->ofport);
        match_set_dl_type(&match, htons(ETH_TYPE_IP));
        match_set_nw_proto(&match, IPPROTO_ICMP);
        match_set_icmp_type(&match, 3);
        match_set_icmp_code(&match, 4);

        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 120, 0, &match,
                        &ofpacts, hc_uuid);
        /* IPv6 */
        match_init_catchall(&match);
        match_set_in_port(&match, tun->ofport);
        match_set_dl_type(&match, htons(ETH_TYPE_IPV6));
        match_set_nw_proto(&match, IPPROTO_ICMPV6);
        match_set_icmp_type(&match, 2);
        match_set_icmp_code(&match, 0);

        ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 120, 0, &match,
                        &ofpacts, hc_uuid);
    }

    /* Add VXLAN specific rules to transform port keys
     * from 12 bits to 16 bits used elsewhere. */
    HMAP_FOR_EACH (tun, hmap_node, p_ctx->chassis_tunnels) {
        if (tun->type == VXLAN) {
            ofpbuf_clear(&ofpacts);

            struct match match = MATCH_CATCHALL_INITIALIZER;
            match_set_in_port(&match, tun->ofport);
            ovs_be64 mcast_bits = htonll((OVN_VXLAN_MIN_MULTICAST << 12));
            match_set_tun_id_masked(&match, mcast_bits, mcast_bits);

            put_load(1, MFF_LOG_OUTPORT, 15, 1, &ofpacts);
            put_move(MFF_TUN_ID, 12, MFF_LOG_OUTPORT,  0, 11, &ofpacts);
            put_move(MFF_TUN_ID, 0, MFF_LOG_DATAPATH, 0, 12, &ofpacts);
            put_resubmit(OFTABLE_LOCAL_OUTPUT, &ofpacts);

            ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 105, 0,
                            &match, &ofpacts, hc_uuid);
        }
    }

    /* Handle ramp switch encapsulations. */
    HMAP_FOR_EACH (tun, hmap_node, p_ctx->chassis_tunnels) {
        if (tun->type != VXLAN) {
            continue;
        }

        SBREC_PORT_BINDING_TABLE_FOR_EACH (binding,
                                           p_ctx->port_binding_table) {
            if (strcmp(binding->type, "vtep")) {
                continue;
            }

            if (!binding->chassis ||
                !encaps_tunnel_id_match(tun->chassis_id,
                                        binding->chassis->name, NULL, NULL)) {
                continue;
            }

            struct match match = MATCH_CATCHALL_INITIALIZER;
            match_set_in_port(&match, tun->ofport);
            ofpbuf_clear(&ofpacts);

            /* Add flows for ramp switches.  The VNI is used to populate
             * MFF_LOG_DATAPATH.  The gateway's logical port is set to
             * MFF_LOG_INPORT.  Then the packet is resubmitted to table 8
             * to determine the logical egress port. */
            match_set_tun_id(&match, htonll(binding->datapath->tunnel_key));

            put_move(MFF_TUN_ID, 0,  MFF_LOG_DATAPATH, 0, 24, &ofpacts);
            put_load(binding->tunnel_key, MFF_LOG_INPORT, 0, 15, &ofpacts);
            /* For packets received from a ramp tunnel, set a flag to that
             * effect. */
            put_load(1, MFF_LOG_FLAGS, MLF_RCV_FROM_RAMP_BIT, 1, &ofpacts);
            put_resubmit(OFTABLE_LOG_INGRESS_PIPELINE, &ofpacts);

            ofctrl_add_flow(flow_table, OFTABLE_PHY_TO_LOG, 110,
                            binding->header_.uuid.parts[0],
                            &match, &ofpacts, hc_uuid);
        }
    }

    /* Table 0, priority 0.
     * ======================
     *
     * Drop packets tha do not match any tunnel in_port.
     */
    add_default_drop_flow(p_ctx, OFTABLE_PHY_TO_LOG, flow_table);

    /* Table 40-43, priority 0.
     * ========================
     *
     * Default resubmit actions for OFTABLE_OUTPUT_LARGE_PKT_* tables.
     */
    struct match match;
    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_REMOTE_OUTPUT, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_OUTPUT_LARGE_PKT_DETECT, 0, 0, &match,
                    &ofpacts, hc_uuid);

    match_init_catchall(&match);
    match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                         MLF_ALLOW_LOOPBACK, MLF_ALLOW_LOOPBACK);
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_LOCAL_OUTPUT, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_OUTPUT_LARGE_PKT_PROCESS, 10, 0,
                    &match, &ofpacts, hc_uuid);

    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_REMOTE_OUTPUT, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_OUTPUT_LARGE_PKT_PROCESS, 0, 0, &match,
                    &ofpacts, hc_uuid);

    /* Table 42, priority 150.
     * =======================
     *
     * Handles packets received from a VXLAN tunnel which get resubmitted to
     * OFTABLE_LOG_INGRESS_PIPELINE due to lack of needed metadata in VXLAN,
     * explicitly skip sending back out any tunnels and resubmit to table 40
     * for local delivery, except packets which have MLF_ALLOW_LOOPBACK bit
     * set.
     */
    match_init_catchall(&match);
    match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0, MLF_RCV_FROM_RAMP,
                         MLF_RCV_FROM_RAMP | MLF_ALLOW_LOOPBACK);

    /* Resubmit to table 40. */
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_LOCAL_OUTPUT, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_REMOTE_OUTPUT, 150, 0,
                    &match, &ofpacts, hc_uuid);

    /* Table 42, priority 150.
     * =======================
     *
     * Packets that should not be sent to other hypervisors.
     */
    match_init_catchall(&match);
    match_set_reg_masked(&match, MFF_LOG_FLAGS - MFF_REG0,
                         MLF_LOCAL_ONLY, MLF_LOCAL_ONLY);
    /* Resubmit to table 40. */
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_LOCAL_OUTPUT, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_REMOTE_OUTPUT, 150, 0,
                    &match, &ofpacts, hc_uuid);

    /* Table 42, Priority 0.
     * =======================
     *
     * Resubmit packets that are not directed at tunnels or part of a
     * multicast group to the local output table. */
    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_LOCAL_OUTPUT, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_REMOTE_OUTPUT, 0, 0, &match,
                    &ofpacts, hc_uuid);

    /* Table 40, priority 0.
     * ======================
     *
     * Drop packets that do not match previous flows.
     */
    add_default_drop_flow(p_ctx, OFTABLE_LOCAL_OUTPUT, flow_table);

    /* Table 41, Priority 0.
     * =======================
     *
     * Resubmit packets that don't output to the ingress port (already checked
     * in table 40) to the logical egress pipeline, clearing the logical
     * registers (for consistent behavior with packets that get tunneled). */
    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);
    for (int i = 0; i < MFF_N_LOG_REGS; i++) {
        put_load(0, MFF_REG0 + i, 0, 32, &ofpacts);
    }
    put_resubmit(OFTABLE_LOG_EGRESS_PIPELINE, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CHECK_LOOPBACK, 0, 0, &match,
                    &ofpacts, hc_uuid);

    /* Table 64, Priority 0.
     * =======================
     *
     * Resubmit packets that do not have the MLF_ALLOW_LOOPBACK flag set
     * to table 65 for logical-to-physical translation. */
    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);
    put_resubmit(OFTABLE_LOG_TO_PHY, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_SAVE_INPORT, 0, 0, &match,
                    &ofpacts, hc_uuid);

    /* Table 65, priority 0.
     * ======================
     *
     * Drop packets that do not match previous flows.
     */
    add_default_drop_flow(p_ctx, OFTABLE_LOG_TO_PHY, flow_table);

    /* Table 81, 82 and 83
     * Match on ct.trk and ct.est | ct.new and store the ct_nw_dst, ct_ip6_dst,
     * ct_tp_dst and ct_proto in the registers. */
    uint32_t ct_state_est = OVS_CS_F_TRACKED | OVS_CS_F_ESTABLISHED;
    uint32_t ct_state_new = OVS_CS_F_TRACKED | OVS_CS_F_NEW;
    struct match match_new = MATCH_CATCHALL_INITIALIZER;
    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);

    /* Add the flows:
     * match = (ct.trk && ct.est), action = (<reg_result> = ct_tp_dst)
     * table = 83
     * match = (ct.trk && ct.new), action = (<reg_result> = ct_tp_dst)
     * table = 83
     */
    match_set_ct_state_masked(&match, ct_state_est, ct_state_est);
    put_move(MFF_CT_TP_DST, 0,  MFF_LOG_RESULT_REG, 0, 16, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_TP_DST_LOAD, 100, 0, &match,
                    &ofpacts, hc_uuid);

    match_set_ct_state_masked(&match_new, ct_state_new, ct_state_new);
    put_move(MFF_CT_TP_DST, 0,  MFF_LOG_RESULT_REG, 0, 16, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_TP_DST_LOAD, 100, 0,
                    &match_new, &ofpacts, hc_uuid);

    /* Add the flows:
     * match = (ct.trk && ct.est), action = (<reg_result> = ct_proto)
     * table = 86
     * match = (ct.trk && ct.new), action = (<reg_result> = ct_proto)
     * table = 86
     */
     ofpbuf_clear(&ofpacts);
     put_move(MFF_CT_NW_PROTO, 0,  MFF_LOG_RESULT_REG, 0, 8, &ofpacts);
     ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_PROTO_LOAD, 100, 0, &match,
                     &ofpacts, hc_uuid);

     put_move(MFF_CT_NW_PROTO, 0,  MFF_LOG_RESULT_REG, 0, 8, &ofpacts);
     ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_PROTO_LOAD, 100, 0,
                     &match_new, &ofpacts, hc_uuid);
    /* Add the flows:
     * match = (ct.trk && ct.est && ip4), action = (<reg_result> = ct_nw_dst)
     * table = 81
     * match = (ct.trk && ct.new && ip4), action = (<reg_result> = ct_nw_dst)
     * table = 81
     */
    ofpbuf_clear(&ofpacts);
    match_set_dl_type(&match, htons(ETH_TYPE_IP));
    put_move(MFF_CT_NW_DST, 0,  MFF_LOG_RESULT_REG, 0, 32, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_NW_DST_LOAD, 100, 0, &match,
                    &ofpacts, hc_uuid);

    match_set_dl_type(&match_new, htons(ETH_TYPE_IP));
    put_move(MFF_CT_NW_DST, 0,  MFF_LOG_RESULT_REG, 0, 32, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_NW_DST_LOAD, 100, 0,
                    &match_new, &ofpacts, hc_uuid);

    /* Add the flows:
     * match = (ct.trk && ct.est && ip6), action = (<reg_result> = ct_ip6_dst)
     * table = 82
     * match = (ct.trk && ct.new && ip6), action = (<reg_result> = ct_ip6_dst)
     * table = 82
     */
    ofpbuf_clear(&ofpacts);
    match_set_dl_type(&match, htons(ETH_TYPE_IPV6));
    put_move(MFF_CT_IPV6_DST, 0,  MFF_LOG_RESULT_REG, 0,
             128, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_IP6_DST_LOAD, 100, 0, &match,
                    &ofpacts, hc_uuid);

    match_set_dl_type(&match_new, htons(ETH_TYPE_IPV6));
    put_move(MFF_CT_IPV6_DST, 0,  MFF_LOG_RESULT_REG, 0,
             128, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_ORIG_IP6_DST_LOAD, 100, 0,
                    &match_new, &ofpacts, hc_uuid);

    /* Implement the ct_state_save() logical action. */

    /* Add the flow:
     * match = (1), action = (reg4[0..7] = ct_state[0..7])
     * table = 85, priority = UINT16_MAX - 1
     */
    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);
    put_move(MFF_CT_STATE, 0,  MFF_LOG_CT_SAVED_STATE, 0, 8, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_STATE_SAVE, UINT16_MAX - 1, 0,
                    &match, &ofpacts, hc_uuid);

    /* Add the flow:
     * match = (!ct.trk), action = (reg4[0..7] = 0)
     * table = 85, priority = UINT16_MAX
     */
    match_init_catchall(&match);
    ofpbuf_clear(&ofpacts);
    match_set_ct_state_masked(&match, 0, OVS_CS_F_TRACKED);
    put_load(0, MFF_LOG_CT_SAVED_STATE, 0, 8, &ofpacts);
    ofctrl_add_flow(flow_table, OFTABLE_CT_STATE_SAVE, UINT16_MAX, 0,
                    &match, &ofpacts, hc_uuid);

    physical_eval_remote_chassis_flows(p_ctx, &ofpacts, flow_table);

    ofpbuf_uninit(&ofpacts);
}
