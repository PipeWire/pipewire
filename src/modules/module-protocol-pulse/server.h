/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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
