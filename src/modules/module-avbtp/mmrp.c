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

#include <pipewire/pipewire.h>

#include "mmrp.h"

static const uint8_t mac[6] = AVB_MMRP_MAC;

struct mmrp {
	struct server *server;
	struct spa_hook server_listener;
};

static int mmrp_message(void *data, uint64_t now, const void *message, int len)
{
	const struct avbtp_packet_mrp *p = message;

	if (ntohs(p->eth.type) != AVB_MMRP_ETH)
		return 0;
	if (memcmp(p->eth.dest, mac, 6) != 0)
		return 0;

	pw_log_info("MMRP");

	return 0;
}

static void mmrp_destroy(void *data)
{
	struct mmrp *mmrp = data;
	spa_hook_remove(&mmrp->server_listener);
	free(mmrp);
}

static const struct server_events server_events = {
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = mmrp_destroy,
	.message = mmrp_message
};

int avbtp_mmrp_register(struct server *server)
{
	struct mmrp *mmrp;

	mmrp = calloc(1, sizeof(*mmrp));
	if (mmrp == NULL)
		return -errno;

	mmrp->server = server;

	avdecc_server_add_listener(server, &mmrp->server_listener, &server_events, mmrp);

	return 0;
}
