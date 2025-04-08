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
    /** persisted */
    const bool is_persited;
    /** The descriptor type it belongs to */
    const uint16_t desc_type;
    /** Not all var can time out */
    bool expires;
    /** The counts of the vars per entity */
    size_t      count;
    /** Element size */
    size_t      el_sz;
};

/**
 * Basic information about the last time it was updated, or will expired
 * and the controller that last accessed it
 */
struct aecp_aem_base_info {
    struct aem_state_var_info var_info;
    /** Originator of the control
     * This is needed so the unsoolictied notification does not send back SUCCESS
     * to the originator of of the unsolicited notification */
    uint64_t controller_entity_id;

    /** Check the need for an update, that is usually updated when setting var. */
    bool needs_update;

    /** timeout absolute time*/
    int64_t expire_timeout;
};

struct aecp_aem_lock_state {
    struct aecp_aem_base_info base_info;
    /**
     * The entity id that is locking this system
     */
    uint64_t locked_id;

    /**
     * Actual value of the lock
     */
    bool is_locked;
};

struct aecp_aem_unsol_notification_state {
    struct aecp_aem_base_info base_info;
    /**
     * The controller is that is locking this system
     */
    uint64_t ctrler_endity_id;

    /**
     * mac Address of the controller
     */
    uint8_t ctrler_mac_addr[6];

    /**
     * Port where the registeration originated from
     */
    uint8_t port_id;

    /***
     * The sequence ID of the next unsollicited notification
     */

    uint16_t next_seq_id;
    /**
     * Actual value of the lock, get removed when unregistere or expired.
     */
    bool is_registered;

};


enum aecp_aem_lock_types {
    aecp_aem_min = -1, // Sentinel check

	aecp_aem_lock,
    aecp_aem_unsol_notif,

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
 * @return -1 if the var does not exists or if the type is not sypported yet.
 */
int aecp_aem_get_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, uint16_t id, void *state);

/**
 * @brief this function will just refresh hte variable, not setting additional
 *      the metadata such as teh controoler Id that accessed it lastly and the
 *      needs_udpate value
 */
int aecp_aem_refresh_state_var(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, uint16_t id, void *state);

/**
 * @brief retrieves the base information about a variable
 */
int aecp_aem_get_base_info(struct aecp* aecp, uint64_t target_id,
    enum aecp_aem_lock_types type, uint16_t id, struct aecp_aem_base_info **info);

/**
 * @brief Retrieve the variable holding the state of a specific information of
 *          the AECP states.
 * @return 0 upon success, -1 if the variable or type are invalid.
 */
int aecp_aem_set_state_var(struct aecp* aecp, uint64_t target_id,
    uint64_t ctrler_id, enum aecp_aem_lock_types type, uint16_t id, void *state);

/**
 * TODO
 */
int aecp_aem_init_var_containers(struct aecp *aecp,
        const struct aem_state_var_info *varsdesc, size_t array_size);

#endif // AVB_AECP_AEM_STATE_H