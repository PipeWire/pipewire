/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#include <limits.h>
#include <stdbool.h>
#include <inttypes.h>

#include "../aecp.h"
#include "../aecp-aem.h"
#include "../aecp-aem-state.h"
#include "../aecp-aem-descriptors.h"
#include "../aecp-aem-types.h"

#include "cmd-resp-helpers.h"
#include "cmd-get-set-sampling-rate.h"

#include "reply-unsol-helpers.h"

/**
 * \brief Verify if the audio unit supports the sample in the list it has
 * \param const uint32_t samplerate: The sample rate command received
 * \return true if the samplerate provide is supported by the audio unit
 * 	 false otherwise.
 */
static bool valid_sample_rate_audio_unit_compat(const struct avb_aem_desc_audio_unit *au,
	const union avb_packet_aecp_aem_pull_frequency *pullfreq)
{
	uint32_t au_pull_freq;
	// Descriptors are always are using the network endianees.
	uint16_t au_sr_counts = ntohs(au->sampling_rates_count);
	const union avb_aem_desc_sampling_rate *sr;

	/* TODO: it's necessary to handle the pull ?!!!*/
	for (uint32_t pos = 0; pos < au_sr_counts; pos++) {
		sr = &au->sampling_rates[pos];
		au_pull_freq = ntohl(sr->pull_frequency);

		if (au_pull_freq == pullfreq->pull_frequency) {
			return true;
		}
	}

	pw_log_error("Unsupported Audio Unit sample rate %d\n",
		pullfreq->frequency);

	return false;
}

/**
 * \brief Verify if the samplerate provide is supported by Milan V1.2
 * \param const union pullfreq: union holding all the necessary information.
 * 	the function expects the Host endianess.
 * \return true if the samplerate provide is supported by Milan V1.2
 * 	 false otherwise.
 */
static bool valid_sample_rate_milan_v12(const union avb_packet_aecp_aem_pull_frequency *pullfreq)
{
	static const uint32_t list_valid_samplerates[] = {
		192000, 96000, 48000
	};

	static const size_t elmt = SPA_N_ELEMENTS(list_valid_samplerates);

	/* TODO: it's necessary to handle the pull ?!!!*/
	for (uint32_t pos = 0; pos < elmt; pos++) {
		if (list_valid_samplerates[pos] ==  pullfreq->frequency) {
			return true;
		}
	}

	pw_log_error("Unsupported sample rate for Milan V1.2 %d",
		pullfreq->frequency);

	return false;
}

static int send_unsol_get_sampling_rate_milan_v12(struct aecp *aecp,
	const uint8_t *m, size_t len, uint64_t ctrler_id)
{
	uint8_t unsol_buf[512];
	struct aecp_aem_base_info info = { 0 };
	int rc = 0;

	memcpy(unsol_buf, m, len);
	/* Prepare a template packet */
	info.controller_entity_id = htobe64(ctrler_id);
	info.expire_timeout = INT64_MAX;

	rc = reply_unsolicited_notifications(aecp, &info, unsol_buf, len, false);
	return rc;
}

/**
 * \brief Handling errors. The standard forces us to return the current value
 * if the entity does not support the requested sampling rate for the audio
 * unit.
 */
static int sample_rate_invalid_response(struct aecp *aecp,
	const struct avb_aem_desc_audio_unit *unit, const uint8_t *m, size_t len)
{
	uint8_t buf[512];
	struct avb_ethernet_header *h = (struct avb_ethernet_header *) buf;
	struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem_setget_sampling_rate *cmd;

	memcpy(buf, m, len);
	cmd = (struct avb_packet_aecp_aem_setget_sampling_rate *)p->payload;

	memcpy(&cmd->sampling_rate, &unit->current_sampling_rate,
			sizeof(unit->current_sampling_rate));

	return reply_status(aecp, AVB_AECP_AEM_STATUS_NOT_SUPPORTED, p, len);
}

/**
 * \brief Milan sampling rate handles. it sets an internal sampling rate to
 * the audio unit and not the sampling rate received from the network.
 * \sa Milan V1.2 5.4.2.13 SET_SAMPLING_RATE
 * \sa IEEE 1722.1-2021 7.4.21 SET_SAMPLING_RATE
 */
int handle_cmd_set_sampling_rate_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_setget_sampling_rate *cmd;
	union avb_packet_aecp_aem_pull_frequency pullfreq;
	struct descriptor *desc;
	uint16_t desc_type, desc_id;
	int rc;

	/* TODO: take care of the multiplier in the sampling rate.
	 *  This function does not take care call the PULL
	 */
	cmd = (const struct avb_packet_aecp_aem_setget_sampling_rate *)p->payload;
	desc_type = ntohs(cmd->descriptor_type);
	desc_id = ntohs(cmd->descriptor_id);

	memcpy(&pullfreq, &cmd->sampling_rate, sizeof(pullfreq));
	pullfreq.pull_frequency = ntohl(pullfreq.pull_frequency);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type == AVB_AEM_DESC_AUDIO_UNIT) {
		struct avb_aem_desc_audio_unit *unit = desc->ptr;
		/* TODO check if the STREAM_PORT associated with it supportes
		 * the SSRC/ASRC bit.
		 */

		if (!valid_sample_rate_audio_unit_compat(unit, &pullfreq)) {
			return sample_rate_invalid_response(aecp, unit, m, len);
		}

		if (!valid_sample_rate_milan_v12(&pullfreq)) {
			return sample_rate_invalid_response(aecp, unit, m, len);
		}

		memcpy(&unit->current_sampling_rate, &cmd->sampling_rate,
			sizeof(unit->current_sampling_rate));

	} else {
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED, p, len);
	}

	rc = reply_success(aecp, m, len);
	if (rc) {
		return rc;
	}

	return send_unsol_get_sampling_rate_milan_v12(aecp, m, len,
			p->aecp.controller_guid);
}

 /**
  * \brief  Setting the sample rate for the common AVB
  * \sa IEEE 1722.1-2021 7.4.21 SET_SAMPLING_RATE
  */
int handle_cmd_get_sampling_rate_common(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	/** Choose to have ahte maximum power of 2 size that a buffer can hold  */
	/** TODO: In the future this could be optimized to know if the payload
	 * response fits into the buffer */
	uint8_t buf[2048];
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *reply_p;
	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem_setget_sampling_rate *reply_payload;
	uint16_t desc_type, desc_id;
	struct descriptor *desc;
	union avb_packet_aecp_aem_pull_frequency pullfreq;

	const struct avb_packet_aecp_aem_setget_sampling_rate *cmd =
		(const struct avb_packet_aecp_aem_setget_sampling_rate *)p->payload;

	desc_type = ntohs(cmd->descriptor_type);
	desc_id = ntohs(cmd->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type == AVB_AEM_DESC_AUDIO_UNIT) {
		struct avb_aem_desc_audio_unit *unit = desc->ptr;
		memcpy(&pullfreq, &unit->current_sampling_rate, sizeof(pullfreq));
	} else {
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED, p, len);
	}

	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *)buf;
	reply_p = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	reply_payload =
		(struct avb_packet_aecp_aem_setget_sampling_rate *)reply_p->payload;

	memcpy(&reply_payload->sampling_rate, &pullfreq, sizeof(pullfreq));

	return reply_success(aecp, buf, len);
}
