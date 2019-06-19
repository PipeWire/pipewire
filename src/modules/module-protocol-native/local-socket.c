/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>

static const char *
get_remote(const struct pw_properties *properties)
{
	const char *name = NULL;

	if (properties)
		name = pw_properties_get(properties, PW_REMOTE_PROP_REMOTE_NAME);
	if (name == NULL)
		name = getenv("PIPEWIRE_REMOTE");
	if (name == NULL)
		name = "pipewire-0";
	return name;
}

int pw_protocol_native_connect_local_socket(struct pw_protocol_client *client,
					    void (*done_callback) (void *data, int res),
					    void *data)
{
	struct pw_remote *remote = client->remote;
	struct sockaddr_un addr;
	socklen_t size;
	const char *runtime_dir, *name = NULL;
	int res, name_size, fd;

	if ((runtime_dir = getenv("XDG_RUNTIME_DIR")) == NULL) {
		pw_log_error("connect failed: XDG_RUNTIME_DIR not set in the environment");
		res = -EIO;
		goto error;
        }

	name = get_remote(pw_remote_get_properties(remote));

        if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		res = -errno;
		goto error;
	}

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_LOCAL;
        name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", runtime_dir, name) + 1;

        if (name_size > (int) sizeof addr.sun_path) {
                pw_log_error("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
                             runtime_dir, name);
		res = -ENOSPC;
                goto error_close;
        };

        size = offsetof(struct sockaddr_un, sun_path) + name_size;

        if (connect(fd, (struct sockaddr *) &addr, size) < 0) {
		res = -errno;
                goto error_close;
	}

	res = pw_protocol_client_connect_fd(client, fd, true);

	done_callback(data, res);

	return res;

error_close:
	close(fd);
error:
	return res;
}
