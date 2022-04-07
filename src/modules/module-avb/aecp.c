/* AVB support
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <spa/utils/json.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "aecp.h"
#include "aecp-aem.h"
#include "internal.h"

static const uint8_t mac[6] = AVB_BROADCAST_MAC;

struct msg_info {
	uint16_t type;
	const char *name;
	int (*handle) (struct aecp *aecp, const void *p, int len);
};

static int reply_not_implemented(struct aecp *aecp, const void *p, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[len];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);

	memcpy(h, p, len);
	AVB_PACKET_AECP_SET_STATUS(reply, AVB_AECP_STATUS_NOT_IMPLEMENTED);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

static const struct msg_info msg_info[] = {
	{ AVB_AECP_MESSAGE_TYPE_AEM_COMMAND, "aem-command", avb_aecp_aem_handle_command, },
	{ AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE, "aem-response", avb_aecp_aem_handle_response, },
	{ AVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND, "address-access-command", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE, "address-access-response", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_AVC_COMMAND, "avc-command", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_AVC_RESPONSE, "avc-response", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND, "vendor-unique-command", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE, "vendor-unique-response", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_EXTENDED_COMMAND, "extended-command", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_EXTENDED_RESPONSE, "extended-response", NULL, },
};

static inline const struct msg_info *find_msg_info(uint16_t type, const char *name)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(msg_info); i++) {
		if ((name == NULL && type == msg_info[i].type) ||
		    (name != NULL && spa_streq(name, msg_info[i].name)))
			return &msg_info[i];
	}
	return NULL;
}

static int aecp_message(void *data, uint64_t now, const void *message, int len)
{
	struct aecp *aecp = data;
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = message;
	const struct avb_packet_aecp_header *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct msg_info *info;
	int message_type;

	if (ntohs(h->type) != AVB_TSN_ETH)
		return 0;
	if (memcmp(h->dest, mac, 6) != 0 &&
	    memcmp(h->dest, server->mac_addr, 6) != 0)
		return 0;
	if (AVB_PACKET_GET_SUBTYPE(&p->hdr) != AVB_SUBTYPE_AECP)
		return 0;

	message_type = AVB_PACKET_AECP_GET_MESSAGE_TYPE(p);

	info = find_msg_info(message_type, NULL);
	if (info == NULL)
		return reply_not_implemented(aecp, message, len);

	pw_log_debug("got AECP message %s", info->name);

	if (info->handle == NULL)
		return reply_not_implemented(aecp, message, len);

	return info->handle(aecp, message, len);
}

static void aecp_destroy(void *data)
{
	struct aecp *aecp = data;
	spa_hook_remove(&aecp->server_listener);
	free(aecp);
}

static int do_help(struct aecp *aecp, const char *args, FILE *out)
{
	fprintf(out, "{ \"type\": \"help\","
			"\"text\": \""
			  "/adp/help: this help \\n"
			"\" }");
	return 0;
}

static int aecp_command(void *data, uint64_t now, const char *command, const char *args, FILE *out)
{
	struct aecp *aecp = data;
	int res;

	if (!spa_strstartswith(command, "/aecp/"))
		return 0;

	command += strlen("/aecp/");

	if (spa_streq(command, "help"))
		res = do_help(aecp, args, out);
	else
		res = -ENOTSUP;

	return res;
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = aecp_destroy,
	.message = aecp_message,
	.command = aecp_command
};

struct avb_aecp *avb_aecp_register(struct server *server)
{
	struct aecp *aecp;

	aecp = calloc(1, sizeof(*aecp));
	if (aecp == NULL)
		return NULL;

	aecp->server = server;

	avdecc_server_add_listener(server, &aecp->server_listener, &server_events, aecp);

	return (struct avb_aecp*)aecp;
}

void avb_aecp_unregister(struct avb_aecp *aecp)
{
	aecp_destroy(aecp);
}
