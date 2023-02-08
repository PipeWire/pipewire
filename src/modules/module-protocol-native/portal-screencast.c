/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include <pipewire/pipewire.h>

int pw_protocol_native_connect_portal_screencast(struct pw_protocol_client *client,
					    const struct spa_dict *props,
					    void (*done_callback) (void *data, int res),
					    void *data)
{
	return -ENOTSUP;
}
