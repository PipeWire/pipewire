/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_AEM_STATE_H
#define AVB_AECP_AEM_STATE_H

#include <stdint.h>
#include <stdbool.h>

#include "aecp-aem-descriptors.h"
#include "aecp-aem-milan.h"
#include "stream.h"

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
	uint64_t controller_entity_id;

	int64_t last_update;

	/** timeout absolute time*/
	int64_t expire_timeout;
};

/**
 * \brief the structure keeps track of the registered controller entities
 */
struct aecp_aem_unsol_notification_state {
	uint64_t ctrler_entity_id;

	uint8_t ctrler_mac_addr[6];

	uint8_t port_id;


	uint16_t next_seq_id;
	bool is_registered;

};

struct aecp_aem_base_info {
	uint64_t controller_entity_id;

	int64_t last_update;

	/** timeout absolute time*/
	int64_t expire_timeout;
};

struct aecp_aem_lock_state {
	struct aecp_aem_base_info base_info;
	/**
	 * the entity id that is locking this system
	 */
	uint64_t locked_id;

	/**
	 * actual value of the lock
	 */
	bool is_locked;
};

/**
 * \brief Milan v1.2 Section 5.4.2.25 / Tables 5.13–5.14 — AVB Interface counters.
 * LINK_UP / LINK_DOWN / GPTP_GM_CHANGED are mandatory; controllers infer the
 * current link state from (LINK_UP > LINK_DOWN). We don't (yet) hook netlink
 * for runtime up/down events, so link_up is seeded to 1 at descriptor
 * creation — the interface is up by the time the daemon successfully binds. */
struct aecp_aem_avb_interface_counters {
	uint32_t link_up;
	uint32_t link_down;
	uint32_t gptp_gm_changed;
};

struct aecp_aem_avb_interface_state {
	struct avb_msrp_attribute domain_attr;

	struct avb_mvrp_attribute vlan_attr;

	struct aecp_aem_avb_interface_counters counters;

	/* Milan Section 5.4.5: emit unsolicited GET_COUNTERS when any counter is
	 * updated, max once per second per descriptor. counters_dirty is set
	 * by the writer; AECP's periodic emits when dirty AND the rate-limit
	 * has elapsed, then clears dirty and updates last_counters_emit_ns. */
	bool counters_dirty;
	int64_t last_counters_emit_ns;

	bool gptp_info_dirty;
	bool as_path_dirty;
};


/**
 * \brief the generic entity state common for all flavor of AVB
 */
struct aecp_aem_entity_state {
	struct aecp_aem_lock_state lock_state;
};

/**
 * \brief Milan implementation of the entity
 */
struct aecp_aem_entity_milan_state {
	struct aecp_aem_entity_state state;
	struct aecp_aem_unsol_notification_state unsol_notif_state[AECP_AEM_MILAN_MAX_CONTROLLER];
};

/**
 * \brief Legacy AVB implementation of the entity
 */
struct aecp_aem_entity_legacy_avb_state {
	struct aecp_aem_entity_state state;
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

struct stream_common {
	struct stream stream;

	struct avb_msrp_attribute lstream_attr;
	struct avb_msrp_attribute tastream_attr;
	struct avb_msrp_attribute tfstream_attr;
};

struct aecp_aem_stream_input_state {
	struct aecp_aem_stream_input_counters counters;
	struct stream_common common;
	struct avb_mvrp_attribute mvrp_attr;

	/** Milan v1.2 Section 5.3.8.7: started/stopped state of the bound Stream Input.
	 *  Toggled by START_STREAMING / STOP_STREAMING. Defaults to started.
	 *  Undefined when the Stream Input is not bound. */
	bool started;

	bool stream_info_dirty;

	/* Milan Section 5.4.5 counter unsolicited rate-limit (see avb_interface_state). */
	bool counters_dirty;
	int64_t last_counters_emit_ns;

	/* Milan Section 5.4.5.3 / Table 5.16: MEDIA_LOCKED ticks on the first valid
	 * AVTPDU after a silence gap; MEDIA_UNLOCKED ticks when the gap
	 * exceeds AVB_MEDIA_UNLOCK_TIMEOUT_NS. last_frame_rx_ns tracks the
	 * most recent valid PDU; media_locked_state is the current edge. */
	int64_t last_frame_rx_ns;
	bool media_locked_state;
};

struct acmp_stream_status_milan_v12 {
	uint64_t controller_entity_id;
	uint32_t acmp_flags;
	uint8_t probing_status;
	uint8_t acmp_status;
	uint32_t fsm_acmp_state;
	int64_t last_probe_rx_time;
};

/**
 * \brief The Milan v1.2 stream structure needs more information
 * about the different protocol*/
struct aecp_aem_stream_input_state_milan_v12 {
	struct aecp_aem_stream_input_state stream_in_sta;
	struct acmp_stream_status_milan_v12 acmp_sta;
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
	struct aecp_aem_stream_output_counters counters;
	struct stream_common common;

	/** Milan v1.2 Section 4.3.3.1: absolute time of last PROBE_TX_COMMAND received.
	 *  0 = never received. Reset to 0 when SRP is deactivated. */
	int64_t last_probe_rx_time;

	/** Milan v1.2 Section 4.3.3.1: a Listener MSRP attribute matching this Stream
	 *  Output's stream_id is currently registered (foreign declaration
	 *  observed via MRP). Maintained by notify_listener() in msrp.c. */
	bool listener_observed;

	/** Milan v1.2 Section 5.3.7.6: Presentation time offset, in nanoseconds.
	 *  Default 2_000_000 (2 ms). Settable via SET_STREAM_INFO with the
	 *  MSRP_ACC_LAT_VALID flag. Range 0 .. 0x7FFFFFFF. */
	uint32_t presentation_time_offset_ns;

	/** IEEE 1722.1-2021 Section 7.4.39 max_transit_time, nanoseconds. The maximum
	 *  time between an AVTP frame's transmission by this Talker and its
	 *  consumption by any Listener. Read by stream_activate() to seed
	 *  stream->mtt (the per-PDU presentation_time = txtime + mtt) and by
	 *  GET_MAX_TRANSIT_TIME; updated by SET_MAX_TRANSIT_TIME. Default
	 *  2_000_000 (2 ms) — kept in sync with presentation_time_offset_ns
	 *  until the two opcodes are wired up to set them independently. */
	uint64_t max_transit_time_ns;

	bool stream_info_dirty;

	/* Milan Section 5.4.5 counter unsolicited rate-limit (see avb_interface_state). */
	bool counters_dirty;
	int64_t last_counters_emit_ns;
};

struct aecp_aem_stream_output_state_milan_v12 {
	struct aecp_aem_stream_output_state stream_out_sta;
	struct acmp_stream_status_milan_v12 acmp_sta;
};

#endif // AVB_AECP_AEM_STATE_H
