/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_HELPERS_H__
#define __AVB_AECP_AEM_HELPERS_H__

#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>
#include <pipewire/log.h>

#include "../aecp.h"

static inline int reply_status(struct aecp *aecp, int status, const void *m, int len)
{
	uint8_t buf[2048];
	struct server *server = aecp->server;
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);

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

static inline int reply_not_implemented(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("reply not implementing");
	return reply_status(aecp, AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED, m, len);
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
