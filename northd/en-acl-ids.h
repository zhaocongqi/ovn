#ifndef EN_ACL_IDS_H
#define EN_ACL_IDS_H

#include <config.h>
#include <stdbool.h>

#include "lib/inc-proc-eng.h"

void *en_acl_id_init(struct engine_node *, struct engine_arg *);
enum engine_node_state en_acl_id_run(struct engine_node *, void *data);
void en_acl_id_cleanup(void *data);
#endif
