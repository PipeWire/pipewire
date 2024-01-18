/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Dmitry Sharshakov <d3dx12.xx@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_PTP_H
#define PIPEWIRE_PTP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ptp_management_msg {
		// 4 for major_sdo, 4 for msg_type
		uint8_t  major_sdo_id_message_type;
		// 4 for minor, 4 for major
		uint8_t  ver;
		uint16_t message_length_be;
		uint8_t  domain_number;
		uint8_t  minor_sdo_id;
		uint16_t flags_be;
		uint8_t  correction_field[8];
		uint32_t message_type_specific;
		uint8_t  clock_identity[8];
		uint16_t source_port_id_be;
		uint16_t sequence_id_be;
		uint8_t  control_field;
		uint8_t  log_message_interval;
		uint8_t  target_port_identity[8];
		uint16_t target_port_id_be;
		uint8_t  starting_boundary_hops;
		uint8_t  boundary_hops;
		uint8_t  action;
		uint8_t  reserved;
		uint16_t tlv_type_be;
		// length of data after this + 2 for management_id
		uint16_t management_message_length_be;
		uint16_t management_id_be;
} __attribute__((packed));

struct ptp_parent_data_set {
		uint8_t  parent_clock_id[8];
		uint16_t parent_port_id_be;
		uint8_t  parent_stats;
		uint8_t  reserved;
		uint16_t log_variance_be;
		int32_t  phase_change_rate_be;
		uint8_t  gm_prio1;
		uint8_t  gm_clock_class;
		uint8_t  gm_clock_accuracy;
		uint16_t gm_clock_variance_be;
		uint8_t  gm_prio2;
		uint8_t  gm_clock_id[8];
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_PTP_H */
