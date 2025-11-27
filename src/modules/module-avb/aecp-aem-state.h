/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_AEM_STATE_H
#define AVB_AECP_AEM_STATE_H

#include "aecp-aem-descriptors.h"

/**
 * The way structure are organised in a "derived" manner.
 * Each of the state structure must directly "castable" into the descriptor
 * that the state variable relies upon.
 *
 * For instance, if a stream_input descriptor is created, the state structure
 * stream_input_state needs to be created as follow:
 *
 * 	struct stream_input_state {
 * 		struct stream_input_desc ...
 * 		...
 * This way it's possible to get directly from the AEM command the descriptor
 * and the state without having to create a mechanism for this.
 */

/**
 * \brief The base information would be required
 * 	for descriptor that needs to udpate for unsollictied
 * 	notificaction.
 */
struct aecp_aem_state_base {
    /**
     * Originator of the control
     * This is needed so the unsoolictied notification does not send back SUCCESS
     * to the originator of of the unsolicited notification
     */
    uint64_t controller_entity_id;

    /**
     * To avoid sending on every change for unsol notifications, only once a
     * second
     */
    int64_t last_update;

    /** timeout absolute time*/
    int64_t expire_timeout;
};


/**
 * \brief The stream inputs are specified as in the IEEE-1722.1-2021
 * 	Table 7-156
 */
struct aecp_aem_stream_input_counters {
    struct aecp_aem_state_base base_state;

    uint32_t media_locked;
    uint32_t media_unlocked;
    uint32_t stream_interrupted;
    uint32_t seq_mistmatch;
    uint32_t media_reset;
    /** Timestamp Uncertain */
    uint32_t tu;
    uint32_t unsupported_format;
    uint32_t late_timestamp;
    uint32_t early_timestamp;
    uint32_t frame_rx;
};

struct aecp_aem_stream_input_state {
    struct avb_aem_desc_stream desc;

    struct aecp_aem_stream_input_counters counters;
    struct stream stream;
};

struct aecp_aem_stream_output_counters {
    struct aecp_aem_state_base base_state;

    uint32_t stream_start;
    uint32_t stream_stop;
    uint32_t media_reset;
    uint32_t tu;
    uint32_t frame_tx;
};

struct aecp_aem_stream_output_state {
    struct avb_aem_desc_stream desc;

    struct aecp_aem_stream_output_counters counters;
    struct stream stream;
};

#endif // AVB_AECP_AEM_STATE_H
