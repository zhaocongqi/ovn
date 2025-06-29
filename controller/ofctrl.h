/* Copyright (c) 2015, 2016 Nicira, Inc.
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


#ifndef OFCTRL_H
#define OFCTRL_H 1

#include <stdint.h>

#include "openvswitch/meta-flow.h"
#include "ovsdb-idl.h"
#include "hindex.h"
#include "lib/uuidset.h"

struct ovn_extend_table;
struct hmap;
struct match;
struct ofpbuf;
struct ovsrec_bridge;
struct ovsrec_open_vswitch_table;
struct sbrec_meter_table;
struct sbrec_ecmp_nexthop_table;
struct shash;
struct tracked_acl_ids;

struct ovn_desired_flow_table {
    /* Hash map flow table using flow match conditions as hash key.*/
    struct hmap match_flow_table;

    /* SB uuid index for the cross reference nodes that link to the nodes in
     * match_flow_table.*/
    struct hmap uuid_flow_table;

    /* Is flow changes tracked. */
    bool change_tracked;
    /* Tracked flow changes. */
    struct ovs_list tracked_flows;
};

/* Interface for OVN main loop. */
void ofctrl_init(struct ovn_extend_table *group_table,
                 struct ovn_extend_table *meter_table);
bool ofctrl_run(const char *conn_target, int probe_interval,
                const struct ovsrec_open_vswitch_table *ovs_table,
                struct shash *pending_ct_zones,
                struct tracked_acl_ids *tracked_acl_ids);
enum mf_field_id ofctrl_get_mf_field_id(void);
void ofctrl_put(struct ovn_desired_flow_table *lflow_table,
                struct ovn_desired_flow_table *pflow_table,
                struct shash *pending_ct_zones,
                struct shash *current_ct_zones,
                struct hmap *pending_lb_tuples,
                const struct hmap *local_datapaths,
                struct ovsdb_idl_index *sbrec_meter_by_name,
                const struct sbrec_ecmp_nexthop_table *enh_table,
                uint64_t nb_cfg,
                bool lflow_changed,
                bool pflow_changed,
                struct tracked_acl_ids *tracked_acl_ids,
                bool monitor_cond_complete);
bool ofctrl_has_backlog(void);
void ofctrl_wait(void);
void ofctrl_destroy(void);
uint64_t ofctrl_get_cur_cfg(void);

void ofctrl_ct_flush_zone(uint16_t zone_id);

char *ofctrl_inject_pkt(const struct ovsrec_bridge *br_int,
                        const char *flow_s, const struct shash *addr_sets,
                        const struct shash *port_groups,
                        const struct smap *template_vars);

/* Flow table interfaces to the rest of ovn-controller. */

/* Information of IP of an address set used to track a flow that is generated
 * from a logical flow referencing address set(s). */
struct addrset_info {
    const char *name; /* The address set's name. */
    struct in6_addr ip; /* An IP in the address set. */
    struct in6_addr mask; /* The mask of the IP. */
};
void ofctrl_add_flow(struct ovn_desired_flow_table *, uint8_t table_id,
                     uint16_t priority, uint64_t cookie,
                     const struct match *, const struct ofpbuf *ofpacts,
                     const struct uuid *);

void ofctrl_add_or_append_flow(struct ovn_desired_flow_table *,
                               uint8_t table_id, uint16_t priority,
                               uint64_t cookie, const struct match *,
                               const struct ofpbuf *actions,
                               const struct uuid *sb_uuid,
                               uint32_t meter_id,
                               const struct addrset_info *);

void ofctrl_add_flow_metered(struct ovn_desired_flow_table *desired_flows,
                             uint8_t table_id, uint16_t priority,
                             uint64_t cookie, const struct match *match,
                             const struct ofpbuf *actions,
                             const struct uuid *sb_uuid,
                             uint32_t meter_id,
                             const struct addrset_info *);

/* Removes a bundles of flows from the flow table for a specific sb_uuid. The
 * flows are removed only if they are not referenced by any other sb_uuid(s).
 * For flood-removing all related flows referenced by other sb_uuid(s), use
 * ofctrl_flood_remove_flows(). */
void ofctrl_remove_flows(struct ovn_desired_flow_table *,
                         const struct uuid *sb_uuid);

/* The function ofctrl_flood_remove_flows flood-removes flows from the desired
 * flow table for the sb_uuids provided in the flood_remove_nodes argument.
 * For each given sb_uuid in flood_remove_nodes, it removes all the flows
 * generated by the sb_uuid, and if any of the flows are referenced by another
 * sb_uuid, it continues removing all the flows used by that sb_uuid as well,
 * and so on, recursively.
 *
 * It adds all the sb_uuids that are actually removed in the
 * flood_remove_nodes. */
void ofctrl_flood_remove_flows(struct ovn_desired_flow_table *,
                               struct uuidset *flood_remove_nodes);
bool ofctrl_remove_flows_for_as_ip(struct ovn_desired_flow_table *,
                                   const struct uuid *lflow_uuid,
                                   const struct addrset_info *,
                                   size_t expected_count);

void ovn_desired_flow_table_init(struct ovn_desired_flow_table *);
void ovn_desired_flow_table_clear(struct ovn_desired_flow_table *);
void ovn_desired_flow_table_destroy(struct ovn_desired_flow_table *);

void ofctrl_check_and_add_flow_metered(struct ovn_desired_flow_table *,
                                       uint8_t table_id, uint16_t priority,
                                       uint64_t cookie, const struct match *,
                                       const struct ofpbuf *ofpacts,
                                       const struct uuid *, uint32_t meter_id,
                                       const struct addrset_info *,
                                       bool log_duplicate_flow);


bool ofctrl_is_connected(void);
void ofctrl_get_memory_usage(struct simap *usage);

#endif /* controller/ofctrl.h */
