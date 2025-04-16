/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/json.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "aecp.h"
#include "aecp-aem.h"
#include "aecp-aem-controls.h"
#include "internal.h"


static const uint8_t mac[6] = AVB_BROADCAST_MAC;
static const uint8_t mac_identity[6] = BASE_CTRL_IDENTIFY_MAC;

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
	{ AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND, "vendor-unique-command", avb_aecp_vendor_unique_command, },
	{ AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE, "vendor-unique-response", avb_aecp_vendor_unique_response, },
	{ AVB_AECP_MESSAGE_TYPE_EXTENDED_COMMAND, "extended-command", NULL, },
	{ AVB_AECP_MESSAGE_TYPE_EXTENDED_RESPONSE, "extended-response", NULL, },
};

static inline const struct msg_info *find_msg_info(uint16_t type, const char *name)
{
	SPA_FOR_EACH_ELEMENT_VAR(msg_info, i) {
		if ((name == NULL && type == i->type) ||
		    (name != NULL && spa_streq(name, i->name)))
			return i;
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
	bool avdecc_identity;
	bool avdecc_general;
	bool avdecc_entity;
	bool is_control_type;

	if (ntohs(h->type) != AVB_TSN_ETH)
		return 0;

	avdecc_general = memcmp(h->dest, mac, 6) == 0;
	avdecc_identity = memcmp(h->dest, mac_identity, 6) == 0;
	avdecc_entity = memcmp(h->dest, server->mac_addr, 6) == 0;

	if (!avdecc_general && !avdecc_identity && !avdecc_entity) {
		pw_log_error("Not a supported address\n");
		return 0;
	}

	if (AVB_PACKET_GET_SUBTYPE(&p->hdr) != AVB_SUBTYPE_AECP)
		return 0;

	message_type = AVB_PACKET_AECP_GET_MESSAGE_TYPE(p);

	/* Here CONTROLS have different addresses so we need to take care of it */
	is_control_type = (message_type == AVB_AECP_AEM_CMD_SET_CONTROL) ||
						(message_type == AVB_AECP_AEM_CMD_GET_CONTROL);
	// TODO check what should be the appropriate return when the control
	// address is issued and the message_tgype is different than CONTROL
	// Also now we only support identify XOR here
	if (avdecc_identity != is_control_type ) {
		pw_log_error("trying to use identity address without control type\n");
		return reply_not_implemented(aecp, message, len);
	}

	info = find_msg_info(message_type, NULL);
	if (info == NULL)
		return reply_not_implemented(aecp, message, len);

	pw_log_debug("got AECP message %s", info->name);

	if (info->handle == NULL)
		return reply_not_implemented(aecp, message, len);

	// TODO here check if unsolicited change are needed,
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

static void aecp_periodic (void *data, uint64_t now)
{
	struct aecp *aecp = data;
	if (now > aecp->timeout) {
		avb_aecp_aem_handle_timeouts(aecp, now);
	}
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = aecp_destroy,
	.message = aecp_message,
	.command = aecp_command,
	.periodic = aecp_periodic
};

struct avb_aecp *avb_aecp_register(struct server *server)
{
	struct aecp *aecp;

	aecp = calloc(1, sizeof(*aecp));
	if (aecp == NULL)
		return NULL;

	aecp->server = server;

	avdecc_server_add_listener(server, &aecp->server_listener,
								 &server_events, aecp);

	return (struct avb_aecp*)aecp;
}

void avb_aecp_unregister(struct avb_aecp *aecp)
{
	struct aecp *_aecp = (struct aecp*) aecp;

	aecp_destroy(_aecp);
}
