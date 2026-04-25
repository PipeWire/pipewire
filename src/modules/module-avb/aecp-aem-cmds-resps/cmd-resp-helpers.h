/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_HELPERS_H__
#define __AVB_AECP_AEM_HELPERS_H__

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>
#include <pipewire/log.h>

#include "../aecp.h"
#include "../aecp-aem.h"

static inline int reply_status(struct aecp *aecp, int status, const void *m, int len)
{
	uint8_t buf[2048];
	struct server *server = aecp->server;
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);

	if (len < 0 || (size_t)len > sizeof(buf)) {
		pw_log_warn("reply_status: invalid len %d", len);
		return -EINVAL;
	}

	memcpy(buf, m, len);

	pw_log_debug("status 0x%x\n", status);

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(reply, status);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

static inline int reply_entity_locked(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("reply entity locked");
	return reply_status(aecp, AVB_AECP_AEM_STATUS_ENTITY_LOCKED, m, len);
}

/** \brief The function is be directly hooked with the cmd_info structure */
static inline int direct_reply_entiy_locked(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	return reply_entity_locked(aecp, m, len);
}

/* IEEE 1722.1-2013 Section 9.2.1.1.7: NOT_IMPLEMENTED reply carries no AEM payload, CDL=12. */
static inline int reply_not_implemented(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[sizeof(struct avb_ethernet_header) + sizeof(struct avb_packet_aecp_aem)];
	struct avb_ethernet_header *h = (void *)buf;
	struct avb_packet_aecp_aem *reply = SPA_PTROFF(h, sizeof(*h), void);

	pw_log_warn("reply not implementing");

	if ((size_t)len < sizeof(buf))
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED, m, len);

	memcpy(buf, m, sizeof(buf));
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, AVB_PACKET_CONTROL_DATA_OFFSET);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, sizeof(buf));
}

/** \brief The function is be directly hooked with the cmd_info structure */
static inline int direct_reply_not_implemented(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	return reply_not_implemented(aecp, m, len);
}

static inline int reply_not_supported(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("reply not supported");
	return reply_status(aecp, AVB_AECP_AEM_STATUS_NOT_SUPPORTED, m, len);
}

/** \brief The function is be directly hooked with the cmd_info structure */
static inline int direct_reply_not_supported(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	return reply_not_supported(aecp, m, len);
}

static inline int reply_no_resources(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("reply no resources");
	return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_RESOURCES, m, len);
}

/** \brief The function is be directly hooked with the cmd_info structure */
static inline int direct_reply_no_resources(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	return reply_no_resources(aecp, m, len);
}

static inline int reply_bad_arguments(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("reply bad arguments");
	return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);
}

/** \brief The function is be directly hooked with the cmd_info structure */
static inline int direct_reply_bad_arguments(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	return reply_bad_arguments(aecp, m, len);
}

static inline int reply_success(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVB_AECP_AEM_STATUS_SUCCESS, m, len);
}

/** \brief The function is be directly hooked with the cmd_info structure */
static inline int direct_reply_success(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	return reply_success(aecp, m, len);
}

#endif //__AVB_AECP_AEM_HELPERS_H__
