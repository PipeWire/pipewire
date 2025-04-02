/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_AEM_STATE_H
#define AVB_AECP_AEM_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "internal.h"

struct aem_state_var_info {
    /** The name of the var for debug */
    const char *var_name;
    /** The counts of the vars per entity */
    size_t      count;
    /** Element size */
    size_t      el_sz;
};

struct aecp_aem_lock_state {
    /** Timestamp when the lock was set, two approach for handling this,
     * [x] lazyway:
     *      the ADP state machine will be in charge of update the value after
     *     the timestamps are reached.
     *
     * [ ] complicated but better than lazy: use a periodic timer for the AECP state
     * machine. Need more mofidicaiton on the state machines.
     */
    uint64_t timestamp;

    /**
     * The entity id that is locking this system
     */
    uint64_t locked_id;

    /**
     * Actual value of the lock
     */
    bool is_locked;
};

enum aecp_aem_lock_types {
    aecp_aem_min = 0, // Sentinel check

	aecp_aem_lock,

    aecp_aem_max  // Sentinel check
};

/**
 * @brief Creation of the aecm aem variable that keep track of the state of
 * specific variable in the AECP realm
 */
// int aecp_aem_delete(struct aecp* aecp, uint64_t target_id,
//     enum aecp_aem_lock_types type, size_t size);

/**
 * @brief Delete variable from the aecp list
 */
int aecp_aem_delete(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, size_t size);

/**
 * @brief Retrieve the variable holding the state of a specific information of
 *          the AECP states.
 * @return NULL if the var does not exists or if the type is not sypported yet.
 */
void* aecp_aem_get_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type);

/**
 * @brief Retrieve the variable holding the state of a specific information of
 *          the AECP states.
 * @return 0 upon success, -1 if the variable or type are invalid.
 */
int aecp_aem_set_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, void *state);

/**
 * TODO
 */
int aecp_aem_init_var_containers(struct aecp *aecp,
        const struct aem_state_var_info *varsdesc, size_t array_size);

#endif // AVB_AECP_AEM_STATE_H