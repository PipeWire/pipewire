/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSER_SERVER_SERVER_H
#define PULSER_SERVER_SERVER_H

#include <stdint.h>

#include <sys/socket.h>

#include <spa/utils/list.h>
#include <spa/utils/hook.h>

struct impl;
struct pw_array;
struct spa_source;

struct server {
	struct spa_list link;
	struct impl *impl;

	struct sockaddr_storage addr;

	struct spa_source *source;
	struct spa_list clients;

	uint32_t max_clients;
	uint32_t listen_backlog;
	char client_access[64];

	uint32_t n_clients;
	uint32_t wait_clients;
	unsigned int activated:1;
};

int servers_create_and_start(struct impl *impl, const char *addresses, struct pw_array *servers);
void server_free(struct server *server);

#endif /* PULSER_SERVER_SERVER_H */
