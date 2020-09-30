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

#include "config.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#if HAVE_PWD_H
#include <pwd.h>
#endif

#include <spa/utils/result.h>

#include "pipewire/pipewire.h"

#include "media-session.h"

#define NAME		"pulse-bridge"
#define SESSION_KEY	"pulse-bridge"

#define FLAG_SHMDATA			0x80000000LU
#define FLAG_SHMDATA_MEMFD_BLOCK	0x20000000LU
#define FLAG_SHMRELEASE			0x40000000LU
#define FLAG_SHMREVOKE			0xC0000000LU
#define FLAG_SHMMASK			0xFF000000LU
#define FLAG_SEEKMASK			0x000000FFLU
#define FLAG_SHMWRITABLE		0x00800000LU

#define FRAME_SIZE_MAX_ALLOW (1024*1024*16)

struct impl {
	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_loop *loop;
        struct spa_source *source;

	struct spa_list clients;
};

struct descriptor {
	uint32_t length;
	uint32_t channel;
	uint32_t offset_hi;
	uint32_t offset_lo;
	uint32_t flags;
};

enum {
	TAG_INVALID = 0,
	TAG_STRING = 't',
	TAG_STRING_NULL = 'N',
	TAG_U32 = 'L',
	TAG_U8 = 'B',
	TAG_U64 = 'R',
	TAG_S64 = 'r',
	TAG_SAMPLE_SPEC = 'a',
	TAG_ARBITRARY = 'x',
	TAG_BOOLEAN_TRUE = '1',
	TAG_BOOLEAN_FALSE = '0',
	TAG_BOOLEAN = TAG_BOOLEAN_TRUE,
	TAG_TIMEVAL = 'T',
	TAG_USEC = 'U'  /* 64bit unsigned */,
	TAG_CHANNEL_MAP = 'm',
	TAG_CVOLUME = 'v',
	TAG_PROPLIST = 'P',
	TAG_VOLUME = 'V',
	TAG_FORMAT_INFO = 'f',
};

struct data {
	uint8_t *data;
	uint32_t length;
	uint32_t offset;
};

struct client {
	struct spa_list link;
	struct impl *impl;

        struct spa_source *source;

	uint32_t index;
	struct descriptor desc;

#define TYPE_PACKET	0
#define TYPE_MEMBLOCK	1
	uint32_t type;
	struct data data;
};

enum {
	/* Generic commands */
	COMMAND_ERROR,
	COMMAND_TIMEOUT, /* pseudo command */
	COMMAND_REPLY,

	/* CLIENT->SERVER */
	COMMAND_CREATE_PLAYBACK_STREAM,        /* Payload changed in v9, v12 (0.9.0, 0.9.8) */
	COMMAND_DELETE_PLAYBACK_STREAM,
	COMMAND_CREATE_RECORD_STREAM,          /* Payload changed in v9, v12 (0.9.0, 0.9.8) */
	COMMAND_DELETE_RECORD_STREAM,
	COMMAND_EXIT,
	COMMAND_AUTH,
	COMMAND_SET_CLIENT_NAME,
	COMMAND_LOOKUP_SINK,
	COMMAND_LOOKUP_SOURCE,
	COMMAND_DRAIN_PLAYBACK_STREAM,
	COMMAND_STAT,
	COMMAND_GET_PLAYBACK_LATENCY,
	COMMAND_CREATE_UPLOAD_STREAM,
	COMMAND_DELETE_UPLOAD_STREAM,
	COMMAND_FINISH_UPLOAD_STREAM,
	COMMAND_PLAY_SAMPLE,
	COMMAND_REMOVE_SAMPLE,

	COMMAND_GET_SERVER_INFO,
	COMMAND_GET_SINK_INFO,
	COMMAND_GET_SINK_INFO_LIST,
	COMMAND_GET_SOURCE_INFO,
	COMMAND_GET_SOURCE_INFO_LIST,
	COMMAND_GET_MODULE_INFO,
	COMMAND_GET_MODULE_INFO_LIST,
	COMMAND_GET_CLIENT_INFO,
	COMMAND_GET_CLIENT_INFO_LIST,
	COMMAND_GET_SINK_INPUT_INFO,          /* Payload changed in v11 (0.9.7) */
	COMMAND_GET_SINK_INPUT_INFO_LIST,     /* Payload changed in v11 (0.9.7) */
	COMMAND_GET_SOURCE_OUTPUT_INFO,
	COMMAND_GET_SOURCE_OUTPUT_INFO_LIST,
	COMMAND_GET_SAMPLE_INFO,
	COMMAND_GET_SAMPLE_INFO_LIST,
	COMMAND_SUBSCRIBE,

	COMMAND_SET_SINK_VOLUME,
	COMMAND_SET_SINK_INPUT_VOLUME,
	COMMAND_SET_SOURCE_VOLUME,

	COMMAND_SET_SINK_MUTE,
	COMMAND_SET_SOURCE_MUTE,

	COMMAND_CORK_PLAYBACK_STREAM,
	COMMAND_FLUSH_PLAYBACK_STREAM,
	COMMAND_TRIGGER_PLAYBACK_STREAM,

	COMMAND_SET_DEFAULT_SINK,
	COMMAND_SET_DEFAULT_SOURCE,

	COMMAND_SET_PLAYBACK_STREAM_NAME,
	COMMAND_SET_RECORD_STREAM_NAME,

	COMMAND_KILL_CLIENT,
	COMMAND_KILL_SINK_INPUT,
	COMMAND_KILL_SOURCE_OUTPUT,

	COMMAND_LOAD_MODULE,
	COMMAND_UNLOAD_MODULE,

	/* Obsolete */
	COMMAND_ADD_AUTOLOAD___OBSOLETE,
	COMMAND_REMOVE_AUTOLOAD___OBSOLETE,
	COMMAND_GET_AUTOLOAD_INFO___OBSOLETE,
	COMMAND_GET_AUTOLOAD_INFO_LIST___OBSOLETE,

	COMMAND_GET_RECORD_LATENCY,
	COMMAND_CORK_RECORD_STREAM,
	COMMAND_FLUSH_RECORD_STREAM,
	COMMAND_PREBUF_PLAYBACK_STREAM,

	/* SERVER->CLIENT */
	COMMAND_REQUEST,
	COMMAND_OVERFLOW,
	COMMAND_UNDERFLOW,
	COMMAND_PLAYBACK_STREAM_KILLED,
	COMMAND_RECORD_STREAM_KILLED,
	COMMAND_SUBSCRIBE_EVENT,

	/* A few more client->server commands */

	/* Supported since protocol v10 (0.9.5) */
	COMMAND_MOVE_SINK_INPUT,
	COMMAND_MOVE_SOURCE_OUTPUT,

	/* Supported since protocol v11 (0.9.7) */
	COMMAND_SET_SINK_INPUT_MUTE,

	COMMAND_SUSPEND_SINK,
	COMMAND_SUSPEND_SOURCE,

	/* Supported since protocol v12 (0.9.8) */
	COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR,
	COMMAND_SET_RECORD_STREAM_BUFFER_ATTR,

	COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE,
	COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE,

	/* SERVER->CLIENT */
	COMMAND_PLAYBACK_STREAM_SUSPENDED,
	COMMAND_RECORD_STREAM_SUSPENDED,
	COMMAND_PLAYBACK_STREAM_MOVED,
	COMMAND_RECORD_STREAM_MOVED,

	/* Supported since protocol v13 (0.9.11) */
	COMMAND_UPDATE_RECORD_STREAM_PROPLIST,
	COMMAND_UPDATE_PLAYBACK_STREAM_PROPLIST,
	COMMAND_UPDATE_CLIENT_PROPLIST,
	COMMAND_REMOVE_RECORD_STREAM_PROPLIST,
	COMMAND_REMOVE_PLAYBACK_STREAM_PROPLIST,
	COMMAND_REMOVE_CLIENT_PROPLIST,

	/* SERVER->CLIENT */
	COMMAND_STARTED,

	/* Supported since protocol v14 (0.9.12) */
	COMMAND_EXTENSION,
	/* Supported since protocol v15 (0.9.15) */
	COMMAND_GET_CARD_INFO,
	COMMAND_GET_CARD_INFO_LIST,
	COMMAND_SET_CARD_PROFILE,

	COMMAND_CLIENT_EVENT,
	COMMAND_PLAYBACK_STREAM_EVENT,
	COMMAND_RECORD_STREAM_EVENT,

	/* SERVER->CLIENT */
	COMMAND_PLAYBACK_BUFFER_ATTR_CHANGED,
	COMMAND_RECORD_BUFFER_ATTR_CHANGED,

	/* Supported since protocol v16 (0.9.16) */
	COMMAND_SET_SINK_PORT,
	COMMAND_SET_SOURCE_PORT,

	/* Supported since protocol v22 (1.0) */
	COMMAND_SET_SOURCE_OUTPUT_VOLUME,
	COMMAND_SET_SOURCE_OUTPUT_MUTE,

	/* Supported since protocol v27 (3.0) */
	COMMAND_SET_PORT_LATENCY_OFFSET,

	/* Supported since protocol v30 (6.0) */
	/* BOTH DIRECTIONS */
	COMMAND_ENABLE_SRBCHANNEL,
	COMMAND_DISABLE_SRBCHANNEL,

	/* Supported since protocol v31 (9.0)
	 * BOTH DIRECTIONS */
	COMMAND_REGISTER_MEMFD_SHMID,

	COMMAND_MAX
};
struct command {
	int (*run) (struct client *client, uint32_t command, uint32_t tag, struct data *d);
};

static int data_readtype(struct data *d, uint8_t type)
{
	if (d->offset + 1 > d->length)
		return -ENOSPC;
	if (d->data[d->offset] != type)
		return -EINVAL;
	d->offset++;
	return 0;
}

static int data_writetype(struct data *d, uint8_t type)
{
	if (d->offset + 1 > d->length)
		return -ENOSPC;
	d->data[d->offset] = type;
	d->offset++;
	return 0;
}

static int data_writeu8(struct data *d, uint8_t val)
{
	if (d->offset + 1 > d->length)
		return -ENOSPC;
	d->data[d->offset] = val;
	d->offset++;
	return 0;
}

static int data_readu8(struct data *d, uint8_t *val)
{
	if (d->offset + 1 > d->length)
		return -ENOSPC;
	*val = d->data[d->offset];
	d->offset++;
	return 0;
}

static int data_readu32(struct data *d, uint32_t *val)
{
	if (d->offset + 4 > d->length)
		return -ENOSPC;
	memcpy(val, &d->data[d->offset], 4);
	*val = ntohl(*val);
	d->offset += 4;
	return 0;
}

static int data_writeu32(struct data *d, uint32_t val)
{
	if (d->offset + 4 > d->length)
		return -ENOSPC;
	val = htonl(val);
	memcpy(d->data + d->offset, &val, 4);
	d->offset += 4;
	return 0;
}

static int data_getu32(struct data *d, uint32_t *val)
{
	if (data_readtype(d, TAG_U32) < 0)
		return -1;
	return data_readu32(d, val);
}
static int data_putu32(struct data *d, uint32_t val)
{
	if (data_writetype(d, TAG_U32) < 0)
		return -1;
	return data_writeu32(d, val);
}

static int send_data(struct client *client, struct data *d)
{
	struct descriptor desc;

	desc.length = htonl(d->offset);
	desc.channel = htonl(-1);
	desc.offset_hi = 0;
	desc.offset_lo = 0;
	desc.flags = 0;
	write(client->source->fd, &desc, sizeof(desc));
	write(client->source->fd, d->data, d->offset);
	return 0;
}

static int do_command_auth(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	pw_log_info(NAME" %p: AUTH", impl);

	data_putu32(&reply, COMMAND_REPLY);
	data_putu32(&reply, tag);
	data_putu32(&reply, 34);

	return send_data(client, &reply);
}

static int do_set_client_name(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	pw_log_info(NAME" %p: SET_CLIENT_NAME", impl);

	data_putu32(&reply, COMMAND_REPLY);
	data_putu32(&reply, tag);
	data_putu32(&reply, 0);

	return send_data(client, &reply);
}

static int do_subscribe(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	pw_log_info(NAME" %p: SUBSCRIBE", impl);

	data_putu32(&reply, COMMAND_REPLY);
	data_putu32(&reply, tag);

	return send_data(client, &reply);
}

static const struct command commands[COMMAND_MAX] =
{
	[COMMAND_AUTH] = { do_command_auth, },
	[COMMAND_SET_CLIENT_NAME] = { do_set_client_name, },
	[COMMAND_SUBSCRIBE] = { do_subscribe, },
};

static void client_free(struct client *client)
{
	struct impl *impl = client->impl;
	spa_list_remove(&client->link);
	if (client->source)
		pw_loop_destroy_source(impl->loop, client->source);
	free(client);
}

static int handle_packet(struct client *client)
{
	struct impl *impl = client->impl;
	int res = 0;
	uint32_t command, tag;
	struct data *d = &client->data;

	if (data_getu32(d, &command) < 0 ||
	    data_getu32(d, &tag) < 0) {
		res = -EPROTO;
		goto finish;
	}

	pw_log_info(NAME" %p: Received packet command %u tag %u",
			impl, command, tag);

	if (command >= COMMAND_MAX || commands[command].run == NULL) {
		res = -ENOTSUP;
		goto finish;
	}

	commands[command].run(client, command, tag, d);

finish:
	return res;
}

static int handle_memblock(struct client *client)
{
	struct impl *impl = client->impl;
	pw_log_info(NAME" %p: Received memblock of size: %u",
			impl, client->data.length);
	return 0;
}

static int do_read(struct client *client)
{
	struct impl *impl = client->impl;
	void *data;
	size_t size;
	ssize_t r;
	int res = 0;

	if (client->index < sizeof(client->desc)) {
		data = SPA_MEMBER(&client->desc, client->index, void);
		size = sizeof(client->desc) - client->index;
	} else {
		uint32_t idx = client->index - sizeof(client->desc);

		if (client->data.data == NULL) {
			res = -EIO;
			goto error;
		}
		data = SPA_MEMBER(client->data.data, idx, void);
		size = client->data.length - idx;
	}
	while (true) {
		pw_log_info(NAME" %p: read %zd", impl, size);
		if ((r = recv(client->source->fd, data, size, 0)) < 0) {
			if (errno == EINTR)
		                continue;
			res = -errno;
			goto error;
		}
		pw_log_info(NAME" %p: got %zd", impl, r);
		client->index += r;
		break;
	}

	if (client->index == sizeof(client->desc)) {
		uint32_t flags, length, channel;

		flags = ntohl(client->desc.flags);
		if ((flags & FLAG_SHMMASK) != 0) {
			res = -ENOTSUP;
			goto error;
		}

		length = ntohl(client->desc.length);
		if (length > FRAME_SIZE_MAX_ALLOW || length <= 0) {
			pw_log_warn(NAME" %p: Received invalid frame size: %u",
					impl, length);
			res = -EPROTO;
			goto error;
		}
		channel = ntohl(client->desc.channel);
		if (channel == (uint32_t) -1) {
			if (flags != 0) {
				pw_log_warn(NAME" %p: Received packet frame with invalid "
						"flags value.", impl);
				res = -EPROTO;
				goto error;
			}
			client->type = TYPE_PACKET;
		} else {
			client->type = TYPE_MEMBLOCK;
		}
		client->data.data = calloc(1, length);
		client->data.length = length;
		client->data.offset = 0;
	} else if (client->index >= ntohl(client->desc.length) + sizeof(client->desc)) {
		switch (client->type) {
		case TYPE_PACKET:
			res = handle_packet(client);
			break;
		case TYPE_MEMBLOCK:
			res = handle_memblock(client);
			break;
		default:
			res = -EPROTO;
			break;
		}
		client->index = 0;
		free(client->data.data);
		client->data.data = NULL;
	}
error:
	return res;
}

static void
on_client_data(void *data, int fd, uint32_t mask)
{
	struct client *client = data;
	struct impl *impl = client->impl;
	int res;

	if (mask & SPA_IO_HUP) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_ERR) {
		res = -EIO;
		goto error;
	}
	if (mask & SPA_IO_OUT) {
		pw_log_info(NAME" %p: can write", impl);
	}
	if (mask & SPA_IO_IN) {
		pw_log_info(NAME" %p: can read", impl);
		if ((res = do_read(client)) < 0)
			goto error;
	}
	return;

error:
        if (res == -EPIPE)
                pw_log_info(NAME" %p: client %p disconnected", impl, client);
        else
                pw_log_error(NAME" %p: client %p error %d (%s)", impl,
                                client, res, spa_strerror(res));
	client_free(client);
}

static void
on_connect(void *data, int fd, uint32_t mask)
{
        struct impl *impl = data;
        struct sockaddr_un name;
        socklen_t length;
        int client_fd;
	struct client *client;

        length = sizeof(name);
        client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
        if (client_fd < 0) {
                pw_log_error(NAME" %p: failed to accept: %m", impl);
                return;
        }
	pw_log_info(NAME": client connection: %d", fd);

	client = calloc(1, sizeof(struct client));
	if (client == NULL) {
                pw_log_error(NAME" %p: failed to create client: %m", impl);
		goto error_close;
	}
	client->impl = impl;
	client->source = pw_loop_add_io(impl->loop,
					client_fd,
					SPA_IO_ERR | SPA_IO_HUP | SPA_IO_IN,
					true, on_client_data, client);
	spa_list_append(&impl->clients, &client->link);
	return;

error_close:
	close(client_fd);
	return;
}

static const char *
get_runtime_dir(void)
{
	const char *runtime_dir;

	runtime_dir = getenv("PULSE_RUNTIME_PATH");
	if (runtime_dir == NULL)
		runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir == NULL)
		runtime_dir = getenv("HOME");
	if (runtime_dir == NULL) {
		struct passwd pwd, *result = NULL;
		char buffer[4096];
		if (getpwuid_r(getuid(), &pwd, buffer, sizeof(buffer), &result) == 0)
			runtime_dir = result ? result->pw_dir : NULL;
	}
	return runtime_dir;
}

static int create_server(struct impl *impl, const char *name)
{
	const char *runtime_dir;
	socklen_t size;
	struct sockaddr_un addr;
	int name_size, fd, res;

	runtime_dir = get_runtime_dir();

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof(addr.sun_path),
                             "%s/pulse/%s", runtime_dir, name) + 1;
	if (name_size > (int) sizeof(addr.sun_path)) {
		pw_log_error(NAME" %p: %s/%s too long",
					impl, runtime_dir, name);
		res = -ENAMETOOLONG;
		goto error;
	}

	struct stat socket_stat;

	if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		res = -errno;
		goto error;
	}
	if (stat(addr.sun_path, &socket_stat) < 0) {
		if (errno != ENOENT) {
			res = -errno;
			pw_log_error("server %p: stat %s failed with error: %m",
					impl, addr.sun_path);
			goto error_close;
		}
	} else if (socket_stat.st_mode & S_IWUSR || socket_stat.st_mode & S_IWGRP) {
		unlink(addr.sun_path);
	}

	size = offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path);
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		res = -errno;
		pw_log_error(NAME" %p: bind() failed with error: %m", impl);
		goto error_close;
	}
	if (listen(fd, 128) < 0) {
		res = -errno;
		pw_log_error(NAME" %p: listen() failed with error: %m", impl);
		goto error_close;
	}
	impl->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, true, on_connect, impl);
	if (impl->source == NULL) {
		res = -errno;
		pw_log_error(NAME" %p: can't create source: %m", impl);
		goto error_close;
	}
	pw_log_info(NAME" listening on %s", addr.sun_path);
	return 0;

error_close:
	close(fd);
error:
	return res;

}


static void session_destroy(void *data)
{
	struct impl *impl = data;
	struct client *c;

	spa_list_consume(c, &impl->clients, link)
		client_free(c);
	spa_hook_remove(&impl->listener);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.destroy = session_destroy,
};

int sm_pulse_bridge_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->loop = session->loop;
	spa_list_init(&impl->clients);

	sm_media_session_add_listener(impl->session,
			&impl->listener,
			&session_events, impl);

	if ((res = create_server(impl, "native")) < 0)
		return res;

	return 0;
}
