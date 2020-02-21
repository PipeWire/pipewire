/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

static const char *
get_remote(const struct spa_dict *props)
{
	const char *name = NULL;

	if (props)
		name = spa_dict_lookup(props, PW_KEY_REMOTE_NAME);
	if (name == NULL)
		name = getenv("PIPEWIRE_REMOTE");
	if (name == NULL)
		name = "pipewire-0";
	return name;
}

int pw_protocol_native_connect_local_socket(struct pw_protocol_client *client,
					    const struct spa_dict *props,
					    void (*done_callback) (void *data, int res),
					    void *data)
{
	struct sockaddr_un addr;
	socklen_t size;
	const char *runtime_dir, *name = NULL;
	int res, name_size, fd;

	if ((runtime_dir = getenv("XDG_RUNTIME_DIR")) == NULL) {
		pw_log_error("connect failed: XDG_RUNTIME_DIR not set in the environment");
		res = -ENOENT;
		goto error;
        }

	name = get_remote(props);

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
		res = -ENAMETOOLONG;
		goto error_close;
        };

        size = offsetof(struct sockaddr_un, sun_path) + name_size;

        if (connect(fd, (struct sockaddr *) &addr, size) < 0) {
		if (errno == ENOENT)
			errno = EHOSTDOWN;
		res = -errno;
                goto error_close;
	}

	res = pw_protocol_client_connect_fd(client, fd, true);

	if (done_callback)
		done_callback(data, res);

	return res;

error_close:
	close(fd);
error:
	return res;
}
