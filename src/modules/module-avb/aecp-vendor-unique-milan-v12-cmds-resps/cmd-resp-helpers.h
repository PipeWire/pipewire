/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_MVU_HELPERS_H__
#define __AVB_AECP_MVU_HELPERS_H__

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>
#include <pipewire/log.h>

#include "../aecp.h"
#include "../aecp-vendor-unique-milan-v12.h"
#include "../internal.h"

/* Milan v1.2 Section 5.4.3.2.2: r bit is reserved (must be 0) in commands and
 * responses. Command vs response is signalled by AECP message_type. */
static inline int reply_mvu_status(struct aecp *aecp, int status,
		const void *m, int len)
{
	uint8_t buf[2048];
	struct server *server = aecp->server;
	struct avb_ethernet_header *h = (void *)buf;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_vendor_unique *vu_reply;
	int wire = len < 60 ? 60 : len;

	if (len < 0 || (size_t)wire > sizeof(buf)) {
		pw_log_warn("reply_mvu_status: invalid len %d", len);
		return -EINVAL;
	}

	memcpy(buf, m, len);
	if (wire > len)
		memset(buf + len, 0, wire - len);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(reply,
			AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(reply, status);

	if ((size_t)wire >= sizeof(*h) + sizeof(*vu_reply)) {
		vu_reply = SPA_PTROFF(h, sizeof(*h), void);
		((uint8_t *)&vu_reply->command_type)[0] &= 0x7f;
	}

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, wire);
}

static inline int reply_mvu_not_implemented(struct aecp *aecp,
		const void *m, int len)
{
	return reply_mvu_status(aecp, AVB_AECP_STATUS_NOT_IMPLEMENTED, m, len);
}

static inline int direct_reply_mvu_not_implemented(struct aecp *aecp,
		int64_t now, const void *m, int len)
{
	(void)now;
	return reply_mvu_not_implemented(aecp, m, len);
}

static inline int reply_mvu_success(struct aecp *aecp,
		const void *m, int len)
{
	return reply_mvu_status(aecp, AVB_AECP_STATUS_SUCCESS, m, len);
}

#endif /* __AVB_AECP_MVU_HELPERS_H__ */
