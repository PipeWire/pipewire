/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "srp.h"

struct srp {
	struct server *server;
	struct spa_hook server_listener;
};

static void srp_destroy(void *data)
{
	struct srp *srp = data;
	spa_hook_remove(&srp->server_listener);
	free(srp);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = srp_destroy,
};

int avb_srp_register(struct server *server)
{
	struct srp *srp;

	srp = calloc(1, sizeof(*srp));
	if (srp == NULL)
		return -errno;

	srp->server = server;

	avdecc_server_add_listener(server, &srp->server_listener, &server_events, srp);

	return 0;
}
