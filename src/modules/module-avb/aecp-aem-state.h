/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_AEM_STATE_H
#define AVB_AECP_AEM_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "internal.h"

#include "gptp/gptp-defs.h"
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
     * The sequence ID of the next unsolicited notification
     */

    uint16_t next_seq_id;
    /**
     * Actual value of the lock, get removed when unregistere or expired.
     */
    bool is_registered;

};

struct aecp_aem_configuration_state {
    struct aecp_aem_base_info base_info;
    uint16_t cfg_idx;
};

/**
 * The aecp_aem_desc_base inherites from the base
 */
struct aecp_aem_desc_base {
    struct aecp_aem_base_info base_info;
    // TODO clean, config_index is held in the descriptor.
    uint16_t config_index;
    void *desc;
};

/**
 * The aecp_aem_desc_base inherites from the base
 */
struct aecp_aem_desc {
    struct aecp_aem_desc_base base_desc;
};

/** The control information to keep track of the latest changes */
struct aecp_aem_control_state {
    struct aecp_aem_desc_base base_desc;
};

/**
 * To keep track of which desciptor has chaanged
 */
struct aecp_aem_name_state {
    struct aecp_aem_desc_base base_desc;
    uint16_t name_index;
};

/**
 * To keep track of which desciptor has changed  */
struct aecp_aem_stream_format_state {
    struct aecp_aem_desc_base base_desc;
};

/** To keep track of the unsolicited notifications */
struct aecp_aem_clock_domain_state {
    struct aecp_aem_desc_base base_desc;
};

/** To keep track of the unsolicited notifications */
struct aecp_aem_sampling_rate_state {
    struct aecp_aem_desc_base base_desc;
};

/**
 * Milan v1.2 Table 5.13: GET_COUNTERSmandatory AVB Interface counters
 * Table 5.14: GET_COUNTERS optional AVB Interface counters
 */
struct aecp_aem_counter_avb_interface_state {
    struct aecp_aem_desc_base base_desc;

    uint32_t link_up;
    uint32_t link_down;
    uint32_t gptp_gm_changed;
    // optional AVB Interface counters
    uint32_t frame_tx;
    uint32_t frame_rx;
    uint32_t error_crc;
};

/**
 * Milan v1.2 Table 5.15: GET_COUNTERS mandatory AVB Interface counters
 */
 struct aecp_aem_counter_clock_domain_state {
    struct aecp_aem_desc_base base_desc;

    uint32_t locked;
    uint32_t unlocked;
};

/**
 * Milan v1.2 Table 5.16: GET_COUNTERS Stream Input counters
 */
struct aecp_aem_counter_stream_input_state {
    struct aecp_aem_desc_base base_desc;

    uint32_t media_locked;
    uint32_t media_unlocked;
    uint32_t stream_interrupted;
    uint32_t seq_mistmatch;
    uint32_t media_reset;
    uint32_t tu;
    uint32_t unsupported_format;
    uint32_t late_timestamp;
    uint32_t early_timestamp;
    uint32_t frame_rx;
};

/**
 * Milan v1.2 Table 5.17: GET_COUNTERS Stream Output counters
 */
struct aecp_aem_counter_stream_output_state {
    struct aecp_aem_desc_base base_desc;

    uint32_t stream_start;
    uint32_t stream_stop;
    uint32_t media_reset;
    uint32_t tu;
    uint32_t frame_tx;
};

/**
 * IEEE 1722.1-2021 Clause 7.4.41 GET_AS_PATHS
 */
struct aecp_aem_ptp_as_path_state {
    struct aecp_aem_desc_base base_desc;
    uint32_t path_count;
    uint64_t path_trace[GPTP_AVB_HOPS_MAX];
};

/**
 * The aecp_aem_desc_base inherites from the base
 */
enum aecp_aem_lock_types {
    aecp_aem_min = -1, // Sentinel check

	aecp_aem_lock,
    aecp_aem_name,
    aecp_aem_clock_domain,
    aecp_aem_configuration,
    aecp_aem_control,
    aecp_aem_stream_format,
    aecp_aem_sampling_rate,
    aecp_aem_counter_avb_interface,
    aecp_aem_counter_clock_domain,
    aecp_aem_counter_stream_input,
    aecp_aem_counter_stream_output,
    aecp_aem_ptp_as_path,
    aecp_aem_unsol_notif,

    // aecp_aem_desc, This is only used to retrieve the value, dynamic change
    // are directly operated on the descriptor themselves.
    // aecp_aem_conf, /** Keep track of the information
    // aecp_aem_format,
    // aecp_aem_stream_format,
    // aecp_aem_stream_info,
    // aecp_aem_name,
    // aecp_aem_sampling_rate,
    // aecp_aem_clock_source,
    // aecp_aem_control,
    // aecp_asem_streaming,
    /** avb_info and as_path directly taken from running platform */
    // aecp_aem_counters,
    // aecp_aem_audio_map,
    // aecp_dynamic_info,

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
