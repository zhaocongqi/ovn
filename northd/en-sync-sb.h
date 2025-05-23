#ifndef EN_SYNC_SB_H
#define EN_SYNC_SB_H 1

#include "lib/inc-proc-eng.h"

void *en_sync_to_sb_init(struct engine_node *, struct engine_arg *);
enum engine_node_state en_sync_to_sb_run(struct engine_node *, void *data);
void en_sync_to_sb_cleanup(void *data);

void *en_sync_to_sb_addr_set_init(struct engine_node *, struct engine_arg *);
enum engine_node_state en_sync_to_sb_addr_set_run(struct engine_node *,
                                                  void *data);
void en_sync_to_sb_addr_set_cleanup(void *data);

enum engine_input_handler_result
sync_to_sb_addr_set_nb_address_set_handler(struct engine_node *, void *data);
enum engine_input_handler_result
sync_to_sb_addr_set_nb_port_group_handler(struct engine_node *, void *data);


void *en_sync_to_sb_lb_init(struct engine_node *, struct engine_arg *);
enum engine_node_state en_sync_to_sb_lb_run(struct engine_node *, void *data);
void en_sync_to_sb_lb_cleanup(void *data);
enum engine_input_handler_result
sync_to_sb_lb_northd_handler(struct engine_node *, void *data OVS_UNUSED);
enum engine_input_handler_result
sync_to_sb_lb_sb_load_balancer(struct engine_node *, void *data OVS_UNUSED);

void *en_sync_to_sb_pb_init(struct engine_node *, struct engine_arg *);
enum engine_node_state en_sync_to_sb_pb_run(struct engine_node *, void *data);
void en_sync_to_sb_pb_cleanup(void *data);
enum engine_input_handler_result
sync_to_sb_pb_northd_handler(struct engine_node *, void *data OVS_UNUSED);

#endif /* end of EN_SYNC_SB_H */
