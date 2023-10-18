/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#include <pipewire/pipewire.h>
#include <spa/utils/json.h>

#define DEFAULT_SYSTEM_RUNTIME_DIR "/run/pipewire"

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

static const char *
get_remote(const struct spa_dict *props)
{
	const char *name;

	name = getenv("PIPEWIRE_REMOTE");
	if ((name == NULL || name[0] == '\0') && props)
		name = spa_dict_lookup(props, PW_KEY_REMOTE_NAME);
	if (name == NULL || name[0] == '\0')
		name = PW_DEFAULT_REMOTE;
	return name;
}

static const char *
get_runtime_dir(void)
{
	const char *runtime_dir;

	runtime_dir = getenv("PIPEWIRE_RUNTIME_DIR");
	if (runtime_dir == NULL)
		runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir == NULL)
		runtime_dir = getenv("USERPROFILE");
	return runtime_dir;
}

static const char *
get_system_dir(void)
{
	return DEFAULT_SYSTEM_RUNTIME_DIR;
}

static int try_connect(struct pw_protocol_client *client,
		const char *runtime_dir, const char *name,
		void (*done_callback) (void *data, int res),
		void *data)
{
	struct sockaddr_un addr;
	socklen_t size;
	int res, name_size, fd;

	pw_log_info("connecting to '%s' runtime_dir:%s", name, runtime_dir);

	if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		res = -errno;
		goto error;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_LOCAL;
	if (runtime_dir == NULL)
		name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", name) + 1;
	else
		name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", runtime_dir, name) + 1;

	if (name_size > (int) sizeof addr.sun_path) {
		if (runtime_dir == NULL)
			pw_log_error("client %p: socket path \"%s\" plus null terminator exceeds %i bytes",
				client, name, (int) sizeof(addr.sun_path));
		else
			pw_log_error("client %p: socket path \"%s/%s\" plus null terminator exceeds %i bytes",
				client, runtime_dir, name, (int) sizeof(addr.sun_path));
		res = -ENAMETOOLONG;
		goto error_close;
	};

	size = offsetof(struct sockaddr_un, sun_path) + name_size;

	if (connect(fd, (struct sockaddr *) &addr, size) < 0) {
		pw_log_debug("connect to '%s' failed: %m", name);
		if (errno == ENOENT)
			errno = EHOSTDOWN;
		if (errno == EAGAIN) {
			pw_log_info("client %p: connect pending, fd %d", client, fd);
		} else {
			res = -errno;
			goto error_close;
		}
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

static int try_connect_name(struct pw_protocol_client *client,
		const char *name,
		void (*done_callback) (void *data, int res),
		void *data)
{
	const char *runtime_dir;
	int res;

	if (name[0] == '/') {
		return try_connect(client, NULL, name, done_callback, data);
	} else {
		runtime_dir = get_runtime_dir();
		if (runtime_dir != NULL) {
			res = try_connect(client, runtime_dir, name, done_callback, data);
			if (res >= 0)
				return res;
		}
		runtime_dir = get_system_dir();
		if (runtime_dir != NULL)
			return try_connect(client, runtime_dir, name, done_callback, data);
	}

	return -EINVAL;
}

int pw_protocol_native_connect_local_socket(struct pw_protocol_client *client,
					    const struct spa_dict *props,
					    void (*done_callback) (void *data, int res),
					    void *data)
{
	const char *name;
	struct spa_json it[2];
	char path[PATH_MAX];
	int res = -EINVAL;

	name = get_remote(props);
	if (name == NULL)
		return -EINVAL;

	spa_json_init(&it[0], name, strlen(name));

	if (spa_json_enter_array(&it[0], &it[1]) < 0)
		return try_connect_name(client, name, done_callback, data);

	while (spa_json_get_string(&it[1], path, sizeof(path)) > 0) {
		res = try_connect_name(client, path, done_callback, data);
		if (res < 0)
			continue;
		break;
	}

	return res;
}
