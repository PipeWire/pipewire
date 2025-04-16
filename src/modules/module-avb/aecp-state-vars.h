/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef  AECP_STATE_VARS_H_
#define  AECP_STATE_VARS_H_

#include "utils.h"
#include "aecp-aem-state.h"


#define AECP_AEM_NEEDED_VAR(type_var, name_str, persist, expire,    \
                             count_var, element_sz)                 \
	[type_var] = { .var_name = name_str, .is_persited = persist,    \
        .count = count_var, .expires = expire,                  \
        .el_sz = element_sz }

/** TODO in the future, accroding to milan spec, some var may be directly
 * implemented depending on the descriptors created */

/** Such a structure should be only used for a specific entity only */
static const struct aem_state_var_info milan_vars[] = {
    AECP_AEM_NEEDED_VAR(aecp_aem_lock,"lock_ref", false, true, 1,
        sizeof(struct aecp_aem_lock_state)),

    /* The set-name var serves only as a way to send unsolicited notifications*/
    AECP_AEM_NEEDED_VAR(aecp_aem_name, "getset-name", true, false, 1,
        sizeof(struct aecp_aem_name_state)),

    AECP_AEM_NEEDED_VAR(aecp_aem_configuration,"configuration", true, false, 1,
        sizeof(struct aecp_aem_configuration_state)),

    AECP_AEM_NEEDED_VAR(aecp_aem_unsol_notif, "unsol_notif_recorded",false, true,
        16, sizeof(struct aecp_aem_unsol_notification_state)),
};

static inline int init_aecp_state_vars(struct aecp *aecp)
{
    spa_list_init(&aecp->server->aecp_aem_states);
    return aecp_aem_init_var_containers(aecp, milan_vars,
                                            ARRAY_SIZE(milan_vars));
}
#endif //AECP_STATE_VARS_H_