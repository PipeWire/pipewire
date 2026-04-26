/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <inttypes.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-state.h"
#include "../acmp-cmds-resps/acmp-milan-v12.h"
#include "../maap.h"
#include "../mrp.h"

#include "cmd-get-set-stream-info.h"
#include "cmd-resp-helpers.h"
#include "reply-unsol-helpers.h"


/* Milan Section 5.4.2.10.2.1 / Table 5.12 — flags_ex.REGISTERING for Stream Output
 * requires the entity to be DECLARING the Talker attribute. The MRP
 * applicant is "declaring" in any non-observer state (anything other than
 * VO/AO/QO/LO). Returns false safely if mrp is NULL, which happens before
 * stream_activate runs (no foreign listener has probed yet, so we're not
 * declaring anything). */
static inline bool mrp_is_declaring(struct avb_mrp_attribute *mrp)
{
	uint8_t state;
	if (mrp == NULL)
		return false;
	state = avb_mrp_attribute_get_applicant_state(mrp);
	return state != AVB_MRP_VO && state != AVB_MRP_AO &&
	       state != AVB_MRP_QO && state != AVB_MRP_LO;
}

/* Full wire size of a Milan GET_STREAM_INFO response (Figure 5.1):
 *   14 (Ethernet) + 4 (AVTP common hdr) + 20 (target+controller+seq+cmd) +
 *   56 (Milan-shaped setget_stream_info body) = 94 bytes. */
#define AVB_AECP_GET_STREAM_INFO_RESPONSE_LEN \
	(int)(sizeof(struct avb_ethernet_header) + \
		sizeof(struct avb_packet_aecp_aem) + \
		sizeof(struct avb_packet_aecp_aem_setget_stream_info))

/* IEEE 1722.1-2021 Section 9.2.1.1.7: CDL excludes the 12-octet AVTPDU common. */
#define AVB_AECP_GET_STREAM_INFO_CDL \
	(uint16_t)(AVB_AECP_GET_STREAM_INFO_RESPONSE_LEN - \
		sizeof(struct avb_ethernet_header) - \
		sizeof(struct avb_packet_header) - \
		sizeof(uint64_t))

/* Hive (and 1722.1 controllers in general) sends GET_STREAM_INFO as a short
 * command — only the descriptor pair, sometimes plus the flags word. The
 * Milan response has to be the full 94-byte frame regardless. Initialise the
 * extra trailing bytes and patch the AVTP control_data_length so the packet
 * is well-formed on the wire. */
static void prepare_full_response(uint8_t *buf, int in_len)
{
	struct avb_packet_aecp_header *aecp_hdr;
	int total = AVB_AECP_GET_STREAM_INFO_RESPONSE_LEN;

	if (in_len < total)
		memset(buf + in_len, 0, total - in_len);

	aecp_hdr = SPA_PTROFF(buf, sizeof(struct avb_ethernet_header), void);
	AVB_PACKET_SET_LENGTH(&aecp_hdr->hdr, AVB_AECP_GET_STREAM_INFO_CDL);
}

static int build_stream_info_response(struct aecp *aecp, const void *m, int len,
		struct avb_packet_aecp_aem_setget_stream_info *reply,
		uint32_t flags_host, uint32_t flags_ex_host,
		uint8_t pbsta, uint8_t acmpsta,
		uint64_t stream_format_be, uint64_t stream_id_be,
		uint32_t msrp_acc_lat_host, const uint8_t dest_mac[6],
		uint8_t msrp_fail_code, uint64_t msrp_fail_bridge_id_be,
		uint16_t stream_vlan_id_host)
{
	(void)len;
	(void)aecp;
	(void)m;

	reply->flags.flags = htonl(flags_host);
	reply->stream_format = stream_format_be;
	reply->stream_id = stream_id_be;
	reply->msrp_accumulated_latency = htonl(msrp_acc_lat_host);
	memcpy(reply->stream_dest_mac, dest_mac, 6);
	reply->msrp_failure_code = msrp_fail_code;
	reply->reserved = 0;
	reply->msrp_failure_bridge_id = msrp_fail_bridge_id_be;
	reply->stream_vlan_id = htons(stream_vlan_id_host);
	reply->stream_vlan_id_reserved = 0;
	reply->flags_ex.milan_v12.flags_ex = htonl(flags_ex_host);
	reply->flags_ex.milan_v12.pbsta_acmpsta =
		htonl(AVB_AEM_STREAM_INFO_PBSTA_ACMPSTA(pbsta, acmpsta));
	return 0;
}

static void populate_input_response(struct aecp *aecp,
		struct descriptor *desc,
		struct avb_packet_aecp_aem_setget_stream_info *reply)
{
	struct aecp_aem_stream_input_state_milan_v12 *si = desc->ptr;
	const struct avb_aem_desc_stream *stream_body = descriptor_body(desc);
	struct stream_common *sc = &si->stream_in_sta.common;
	struct avb_msrp_attribute *lattr = &sc->lstream_attr;
	struct avb_msrp_attribute *taattr = &sc->tastream_attr;
	struct avb_msrp_attribute *tfattr = &sc->tfstream_attr;
	uint32_t flags_host = 0;
	uint32_t flags_ex_host = 0;
	uint64_t stream_format_be;
	uint64_t stream_id_be = 0;
	uint8_t dest_mac[6] = {0};
	uint16_t vlan_id_host = 0;
	uint32_t msrp_acc_lat = 0;
	uint8_t msrp_fail_code = 0;
	uint64_t msrp_fail_bridge_be = 0;
	bool bound, settled, ta_observed, tf_observed, registering;
	(void)aecp;

	stream_format_be = stream_body->current_format;

	/* Milan v1.2 Section 5.5.3.5: bound iff the listener FSM is not in UNBOUND. */
	bound = (si->acmp_sta.fsm_acmp_state !=
			FSM_ACMP_STATE_MILAN_V12_UNBOUND);

	/* Milan Section 5.3.8.5: settled iff probing has completed. */
	settled = (si->acmp_sta.probing_status == 3);
	(void)settled;

	/* Milan Section 5.3.8.8 / Table 5.10 REGISTERING bit: Talker Advertise/Failed
	 * matching the saved SRP params is currently registered. Treat IN and
	 * LV alike — LV is the 1-s leave-timer transient and recovers on the
	 * next JoinIn (matches the talker-side listener_observed semantics
	 * which stay true until LV→MT). */
	ta_observed = (taattr->mrp != NULL) &&
		(avb_mrp_attribute_get_registrar_state(taattr->mrp) == AVB_MRP_IN ||
		 avb_mrp_attribute_get_registrar_state(taattr->mrp) == AVB_MRP_LV);
	tf_observed = (tfattr->mrp != NULL) &&
		(avb_mrp_attribute_get_registrar_state(tfattr->mrp) == AVB_MRP_IN ||
		 avb_mrp_attribute_get_registrar_state(tfattr->mrp) == AVB_MRP_LV);
	registering = ta_observed || tf_observed;

	/* Stream Input — expose the identity fields unconditionally so a
	 * controller (Hive) can show them even before BIND_RX or before SRP
	 * converges. The actual BOUND state is signalled separately via the
	 * CONNECTED flag and FAST_CONNECT/SAVED_STATE. Values default to 0
	 * when not yet known, which is a valid representation per Milan
	 * Section 5.4.2.10.1.1 (the *_VALID flag indicates the field is meaningful,
	 * not that it is non-zero). */
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_FORMAT_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_ID_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_DEST_MAC_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_VLAN_ID_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_MSRP_ACC_LAT_VALID;

	stream_id_be = lattr->attr.listener.stream_id;
	memcpy(dest_mac, sc->stream.addr, 6);
	vlan_id_host = sc->stream.vlan_id;

	if (bound) {
		flags_host |= AVB_AEM_STREAM_INFO_FLAG_FAST_CONNECT;
		flags_host |= AVB_AEM_STREAM_INFO_FLAG_SAVED_STATE;
		flags_host |= AVB_AEM_STREAM_INFO_FLAG_CONNECTED;
		flags_host |= si->stream_in_sta.started ? 0
			: AVB_AEM_STREAM_INFO_FLAG_STREAMING_WAIT;
	}

	/* Section 5.4.2.10.1: msrp_accumulated_latency comes from the registered
	 * Talker attribute. Prefer Talker Advertise (the success path); fall
	 * back to Talker Failed if only that's been observed. 0 otherwise. */
	if (ta_observed)
		msrp_acc_lat = ntohl(taattr->attr.talker.accumulated_latency);
	else if (tf_observed)
		msrp_acc_lat = ntohl(tfattr->attr.talker_fail.talker.accumulated_latency);

	if (tf_observed) {
		flags_host |= AVB_AEM_STREAM_INFO_FLAG_SRP_REGISTERING_FAILED;
		flags_host |= AVB_AEM_STREAM_INFO_FLAG_MSRP_FAILURE_VALID;
		msrp_fail_code = tfattr->attr.talker_fail.failure_code;
		msrp_fail_bridge_be = tfattr->attr.talker_fail.bridge_id;
	}

	/* Table 5.10 flags_ex: REGISTERING (bit 31) iff Talker Advertise/Failed
	 * matching SRP params is registered. */
	if (registering)
		flags_ex_host |= AVB_AEM_STREAM_INFO_FLAGS_EX_REGISTERING;

	build_stream_info_response(aecp, NULL, 0, reply,
			flags_host, flags_ex_host,
			si->acmp_sta.probing_status, si->acmp_sta.acmp_status,
			stream_format_be, stream_id_be, msrp_acc_lat, dest_mac,
			msrp_fail_code, msrp_fail_bridge_be, vlan_id_host);

	pw_log_debug("populate STREAM_INPUT stream_format=0x%016" PRIx64
			" flags=0x%08x flags_ex=0x%08x pbsta=%u acmpsta=%u "
			"stream_id=0x%016" PRIx64,
			be64toh(stream_format_be),
			flags_host, flags_ex_host,
			si->acmp_sta.probing_status, si->acmp_sta.acmp_status,
			be64toh(stream_id_be));
}

static int handle_get_stream_input(struct aecp *aecp, const void *m, int len,
		struct descriptor *desc)
{
	uint8_t buf[2048];
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_stream_info *reply;

	memcpy(buf, m, len);
	prepare_full_response(buf, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	reply = (struct avb_packet_aecp_aem_setget_stream_info *)p_reply->payload;

	populate_input_response(aecp, desc, reply);

	return reply_success(aecp, buf, AVB_AECP_GET_STREAM_INFO_RESPONSE_LEN);
}

static void populate_output_response(struct aecp *aecp, uint16_t desc_index,
		struct descriptor *desc,
		struct avb_packet_aecp_aem_setget_stream_info *reply)
{
	struct aecp_aem_stream_output_state *so = desc->ptr;
	const struct avb_aem_desc_stream *stream_body = descriptor_body(desc);
	struct stream_common *sc = &so->common;
	struct avb_msrp_attribute *taattr = &sc->tastream_attr;
	const uint8_t zero_mac[6] = {0};
	uint8_t taa_reg, lst_reg;
	uint32_t flags_host = 0;
	uint32_t flags_ex_host = 0;
	uint64_t stream_id_be = 0;
	uint8_t dest_mac[6] = {0};
	uint16_t vlan_id_host = 0;
	bool talker_declaring;
	bool reg_failed_listener;

	taa_reg = avb_mrp_attribute_get_registrar_state(taattr->mrp);
	lst_reg = avb_mrp_attribute_get_registrar_state(sc->lstream_attr.mrp);
	(void)taa_reg;
	(void)lst_reg;

	/* Milan Section 5.4.2.10.2.1 / Table 5.12 — REGISTERING for Stream Output
	 * means: declaring TA or TF AND a matching Listener attribute is
	 * registered. We only declare TA (no TALKER_FAILED), so check
	 * tastream_attr's MRP applicant state. The underlying *_VALID flags
	 * for stream_id/dest_mac/vlan_id remain always 1 per Table 5.11 — the
	 * values are static config known from boot, independent of whether
	 * we have begun declaring on the wire. */
	talker_declaring = mrp_is_declaring(taattr->mrp);
	reg_failed_listener = false;

	/* Table 5.11 — Stream Output flags. STREAM_FORMAT_VALID, the three
	 * stream_*_VALID flags, MSRP_ACC_LAT_VALID, and CONNECTED (BOUND) are
	 * always 1 per Milan Section 5.4.2.10.2. */
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_FORMAT_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_CONNECTED;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_MSRP_ACC_LAT_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_ID_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_DEST_MAC_VALID;
	flags_host |= AVB_AEM_STREAM_INFO_FLAG_STREAM_VLAN_ID_VALID;

	if (memcmp(sc->stream.addr, zero_mac, 6) == 0)
		(void)avb_maap_get_address(aecp->server->maap,
				sc->stream.addr, desc_index);

	stream_id_be = htobe64(sc->stream.id);
	memcpy(dest_mac, sc->stream.addr, 6);
	vlan_id_host = sc->stream.vlan_id;

	if (reg_failed_listener) {
		flags_host |= AVB_AEM_STREAM_INFO_FLAG_SRP_REGISTERING_FAILED;
		flags_host |= AVB_AEM_STREAM_INFO_FLAG_MSRP_FAILURE_VALID;
	}

	/* flags_ex.REGISTERING per Table 5.12: declaring TA/TF AND a matching
	 * Listener attribute is registered. */
	if (talker_declaring && so->listener_observed)
		flags_ex_host |= AVB_AEM_STREAM_INFO_FLAGS_EX_REGISTERING;

	/* Section 5.4.2.10.2: pbsta and acmpsta shall be 0 for Stream Output. */
	build_stream_info_response(aecp, NULL, 0, reply,
			flags_host, flags_ex_host,
			0, 0,
			stream_body->current_format, stream_id_be,
			so->presentation_time_offset_ns, dest_mac,
			0, 0, vlan_id_host);

	pw_log_debug("populate STREAM_OUTPUT stream_format=0x%016" PRIx64
			" flags=0x%08x flags_ex=0x%08x pres_time_off=%u "
			"stream_id=0x%016" PRIx64,
			be64toh(stream_body->current_format),
			flags_host, flags_ex_host,
			so->presentation_time_offset_ns,
			be64toh(stream_id_be));
}

static int handle_get_stream_output(struct aecp *aecp, const void *m, int len,
		uint16_t desc_index, struct descriptor *desc)
{
	uint8_t buf[2048];
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_setget_stream_info *reply;

	memcpy(buf, m, len);
	prepare_full_response(buf, len);
	h_reply = (struct avb_ethernet_header *)buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	reply = (struct avb_packet_aecp_aem_setget_stream_info *)p_reply->payload;

	populate_output_response(aecp, desc_index, desc, reply);

	return reply_success(aecp, buf, AVB_AECP_GET_STREAM_INFO_RESPONSE_LEN);
}

int handle_cmd_get_stream_info_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_setget_stream_info *cmd =
		(const struct avb_packet_aecp_aem_setget_stream_info *)p->payload;
	uint16_t desc_type = ntohs(cmd->descriptor_type);
	uint16_t desc_index = ntohs(cmd->descriptor_index);
	struct descriptor *desc;

	(void)now;

	desc = server_find_descriptor(aecp->server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	if (desc_type == AVB_AEM_DESC_STREAM_INPUT) {
		return handle_get_stream_input(aecp, m, len, desc);
	}
	if (desc_type == AVB_AEM_DESC_STREAM_OUTPUT) {
		return handle_get_stream_output(aecp, m, len, desc_index, desc);
	}

	return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);
}

int handle_cmd_set_stream_info_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_setget_stream_info *cmd =
		(const struct avb_packet_aecp_aem_setget_stream_info *)p->payload;
	uint16_t desc_type = ntohs(cmd->descriptor_type);
	uint16_t desc_index = ntohs(cmd->descriptor_index);
	uint32_t flags_host = ntohl(cmd->flags.flags);
	struct descriptor *desc;
	struct aecp_aem_stream_output_state *so;

	(void)now;

	/* Milan Section 5.4.2.9: SET_STREAM_INFO is not implemented for Stream Inputs. */
	if (desc_type == AVB_AEM_DESC_STREAM_INPUT)
		return reply_not_supported(aecp, m, len);
	if (desc_type != AVB_AEM_DESC_STREAM_OUTPUT)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);

	desc = server_find_descriptor(aecp->server, desc_type, desc_index);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);
	so = desc->ptr;

	/* Section 5.4.2.9: any unsupported sub-command → NOT_SUPPORTED for the whole
	 * command. We support only MSRP_ACC_LAT_VALID. */
	{
		uint32_t supported_mask = AVB_AEM_STREAM_INFO_FLAG_MSRP_ACC_LAT_VALID;
		uint32_t valid_mask =
			AVB_AEM_STREAM_INFO_FLAG_CLASS_B |
			AVB_AEM_STREAM_INFO_FLAG_FAST_CONNECT |
			AVB_AEM_STREAM_INFO_FLAG_SAVED_STATE |
			AVB_AEM_STREAM_INFO_FLAG_STREAMING_WAIT |
			AVB_AEM_STREAM_INFO_FLAG_SUPPORTS_ENCRYPTED |
			AVB_AEM_STREAM_INFO_FLAG_ENCRYPTED_PDU |
			AVB_AEM_STREAM_INFO_FLAG_SRP_REGISTERING_FAILED |
			AVB_AEM_STREAM_INFO_FLAG_CL_ENTRIES_VALID |
			AVB_AEM_STREAM_INFO_FLAG_NO_SRP |
			AVB_AEM_STREAM_INFO_FLAG_UDP |
			AVB_AEM_STREAM_INFO_FLAG_STREAM_VLAN_ID_VALID |
			AVB_AEM_STREAM_INFO_FLAG_CONNECTED |
			AVB_AEM_STREAM_INFO_FLAG_MSRP_FAILURE_VALID |
			AVB_AEM_STREAM_INFO_FLAG_STREAM_DEST_MAC_VALID |
			AVB_AEM_STREAM_INFO_FLAG_MSRP_ACC_LAT_VALID |
			AVB_AEM_STREAM_INFO_FLAG_STREAM_ID_VALID |
			AVB_AEM_STREAM_INFO_FLAG_STREAM_FORMAT_VALID;
		uint32_t requested_subcmds = flags_host & valid_mask;
		if (requested_subcmds & ~supported_mask)
			return reply_not_supported(aecp, m, len);
	}

	if (so->listener_observed)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_STREAM_IS_RUNNING, m, len);

	if (flags_host & AVB_AEM_STREAM_INFO_FLAG_MSRP_ACC_LAT_VALID) {
		uint32_t value = ntohl(cmd->msrp_accumulated_latency);
		/* Section 5.4.2.9: range 0 .. 0x7FFFFFFF nanoseconds. */
		if (value > 0x7FFFFFFFu)
			return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);
		if (so->presentation_time_offset_ns != value) {
			so->presentation_time_offset_ns = value;
			so->stream_info_dirty = true;
		}
	}

	/* Section 5.4.2.9: response echoes the command with the same flags. */
	return reply_success(aecp, m, len);
}

void cmd_get_stream_info_emit_unsol_milan_v12(struct server *server,
		uint16_t desc_type, uint16_t desc_index)
{
	uint8_t buf[AVB_AECP_GET_STREAM_INFO_RESPONSE_LEN];
	struct avb_ethernet_header *h;
	struct avb_packet_aecp_aem *p;
	struct avb_packet_aecp_aem_setget_stream_info *body;
	struct descriptor *desc;
	struct aecp_aem_base_info b_state = { 0 };
	struct aecp aecp_local = { .server = server };
	struct aecp *aecp = &aecp_local;

	desc = server_find_descriptor(aecp->server, desc_type, desc_index);
	if (desc == NULL)
		return;
	if (desc_type != AVB_AEM_DESC_STREAM_INPUT &&
	    desc_type != AVB_AEM_DESC_STREAM_OUTPUT)
		return;

	memset(buf, 0, sizeof(buf));
	h = (struct avb_ethernet_header *)buf;
	p = SPA_PTROFF(h, sizeof(*h), void);
	body = (struct avb_packet_aecp_aem_setget_stream_info *)p->payload;

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&p->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&p->aecp.hdr, AVB_AECP_GET_STREAM_INFO_CDL);
	p->cmd1 = 0;
	p->cmd2 = AVB_AECP_AEM_CMD_GET_STREAM_INFO;

	body->descriptor_type = htons(desc_type);
	body->descriptor_index = htons(desc_index);

	if (desc_type == AVB_AEM_DESC_STREAM_INPUT) {
		populate_input_response(aecp, desc, body);
	} else {
		populate_output_response(aecp, desc_index, desc, body);
	}

	(void)reply_unsolicited_notifications(aecp, &b_state, buf,
			AVB_AECP_GET_STREAM_INFO_RESPONSE_LEN, true);
}
