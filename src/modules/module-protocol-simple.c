/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/pod/pod.h>
#include <spa/pod/builder.h>
#include <spa/debug/types.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>

#include <pipewire/impl.h>

#include "network-utils.h"

/** \page page_module_protocol_simple Protocol Simple
 *
 * The simple protocol provides a bidirectional audio stream on a network
 * socket.
 *
 * It is meant to be used with the `simple protocol player` app, available on
 * Android to play and record a stream.
 *
 * Each client that connects will create a capture and/or playback stream,
 * depending on the configuration options.
 *
 * You can also use it to feed audio data to other clients such as the snapcast
 * server.
 *
 * ## Module Name
 *
 * `libpipewire-module-protocol-simple`
 *
 * ## Module Options
 *
 *  - `capture`: boolean if capture is enabled. This will create a capture stream or
 *               sink for each connected client.
 *  - `playback`: boolean if playback is enabled. This will create a playback or
 *               source stream for each connected client.
 *  - `local.ifname = <str>`: interface name to use
 *  - `local.ifaddress = <str>`: interface address to use
 *  - `server.address = []`: an array of server addresses to listen on as
 *                            tcp:(<ip>:)<port>.
 *  - `capture.props`: optional properties for the capture stream
 *  - `playback.props`: optional properties for the playback stream
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_RATE
 * - \ref PW_KEY_STREAM_CAPTURE_SINK
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_TARGET_OBJECT
 *
 * By default the server will work with stereo 16 bits samples at 44.1KHz.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-protocol-simple.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-protocol-simple
 *     args = {
 *         # Provide capture stream, clients can capture data from PipeWire
 *         capture = true
 *         #
 *         # Provide playback stream, client can send data to PipeWire for playback
 *         playback = true
 *         #
 *         #audio.rate = 44100
 *         #audio.format = S16
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         #
 *         # The addresses this server listens on for new
 *         # client connections
 *         server.address = [
 *             "tcp:4711"
 *         ]
 *         capture.props = {
 *             # The node name or id to use for capture.
 *             #target.object = null
 *             #
 *             # To make the capture stream capture the monitor ports
 *             #stream.capture.sink = false
 *             #
 *             # Make this a sink instead of a capture stream
 *             #media.class = Audio/Sink
 *         }
 *         playback.props = {
 *             # The node name or id to use for playback.
 *             #target.object = null
 *             #
 *             # Make this a source instead of a playback stream
 *             #media.class = Audio/Source
 *         }
 *     }
 * }
 * ]
 *\endcode
 *
 * ## Example configuration for a snapcast server
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-protocol-simple
 *     args = {
 *         # Provide sink
 *         capture = true
 *         audio.rate = 48000
 *         audio.format = S16
 *         audio.channels = 2
 *         audio.position = [ FL FR ]
 *
 *         # The addresses this server listens on for new
 *         # client connections
 *         server.address = [
 *             "tcp:4711"
 *         ]
 *         capture.props = {
 *             # Make this a sink instead of a capture stream
 *             media.class = Audio/Sink
 *         }
 *     }
 * }
 * ]
 *
 * On the snapcast server, add the following to the `snapserver.conf` file:
 *
 *\code{.unparsed}
 * [stream]
 * sampleformat =  48000:16:2
 * source = tcp://127.0.0.1:4711?name=PipeWireSnapcast&mode=client
 *\endcode
 *
 * Snapcast will try to connect to the protocol-simple server and fetch the
 * samples from it. Snapcast tries to reconnect when the connection is somehow
 * broken.
 */

#define NAME "protocol-simple"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_PORT 4711
#define DEFAULT_SERVER "[ \"tcp:"SPA_STRINGIFY(DEFAULT_PORT)"\" ]"

#define DEFAULT_FORMAT "S16LE"
#define DEFAULT_RATE 44100
#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

#define MAX_CLIENTS	10

#define MODULE_USAGE	"( capture=<bool> ) "						\
			"( playback=<bool> ) "						\
			"( remote.name=<remote> ) "					\
			"( node.rate=<1/rate, default:1/"SPA_STRINGIFY(DEFAULT_RATE)"> ) "	\
			"( audio.rate=<sample-rate, default:"SPA_STRINGIFY(DEFAULT_RATE)"> ) "		\
			"( audio.format=<format, default:"DEFAULT_FORMAT"> ) "		\
			"( audio.channels=<channels, default: "SPA_STRINGIFY(DEFAULT_CHANNELS)"> ) "	\
			"( audio.position=<position, default:"DEFAULT_POSITION"> ) "	\
			"( server.address=<[ tcp:(<ip>:)<port>(,...) ], default:"DEFAULT_SERVER"> ) "	\
			"( capture.props={ ... } ) "	\
			"( playback.props={ ... } )"	\

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Implements a simple protocol" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_loop *loop;
	struct pw_context *context;

	struct pw_properties *props;
	struct spa_hook module_listener;
	struct spa_list server_list;

	struct pw_work_queue *work_queue;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;

	char *ifname;
	char *ifaddress;
	bool capture;
	bool playback;

	struct spa_audio_info_raw capture_info;
	struct spa_audio_info_raw playback_info;
	uint32_t capture_frame_size;
	uint32_t playback_frame_size;
};

struct client {
	struct spa_list link;
	struct impl *impl;
	struct server *server;

	struct pw_core *core;
        struct spa_hook core_proxy_listener;

	struct spa_source *source;
	char name[128];

	struct pw_stream *capture;
	struct spa_hook capture_listener;

	struct pw_stream *playback;
	struct spa_hook playback_listener;

	unsigned int disconnect:1;
	unsigned int disconnecting:1;
	unsigned int cleanup:1;
};

struct server {
	struct spa_list link;
	struct impl *impl;

#define SERVER_TYPE_INVALID	0
#define SERVER_TYPE_UNIX	1
#define SERVER_TYPE_TCP		2
	uint32_t type;
	struct sockaddr_storage addr;
	struct spa_source *source;

	struct spa_list client_list;
	uint32_t n_clients;
};

static void client_disconnect(struct client *client)
{
	struct impl *impl = client->impl;

	if (client->disconnect)
		return;

	client->disconnect = true;

	if (client->source)
		pw_loop_destroy_source(impl->loop, client->source);
}

static void client_free(struct client *client)
{
	struct impl *impl = client->impl;

	pw_log_info("%p: client:%p [%s] free", impl, client, client->name);

	client_disconnect(client);

	pw_work_queue_cancel(impl->work_queue, client, SPA_ID_INVALID);

	spa_list_remove(&client->link);
	client->server->n_clients--;

	if (client->capture)
		pw_stream_destroy(client->capture);
	if (client->playback)
		pw_stream_destroy(client->playback);
	if (client->core) {
		client->disconnecting = true;
		spa_hook_remove(&client->core_proxy_listener);
		pw_core_disconnect(client->core);
	}
	free(client);
}


static void on_client_cleanup(void *obj, void *data, int res, uint32_t id)
{
	struct client *c = obj;
	client_free(c);
}

static void client_cleanup(struct client *client)
{
	struct impl *impl = client->impl;
	if (!client->cleanup)  {
		client->cleanup = true;
		pw_work_queue_add(impl->work_queue, client, 0, on_client_cleanup, impl);
	}
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
	return;

error:
        if (res == -EPIPE)
                pw_log_info("%p: client:%p [%s] disconnected", impl, client, client->name);
        else  {
                pw_log_error("%p: client:%p [%s] error %d (%s)", impl,
                                client, client->name, res, spa_strerror(res));
	}
	client_cleanup(client);
}

static void capture_process(void *data)
{
	struct client *client = data;
	struct impl *impl = client->impl;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t size, offset;
	int res;

	if ((buf = pw_stream_dequeue_buffer(client->capture)) == NULL) {
		pw_log_debug("%p: client:%p [%s] out of capture buffers: %m", impl,
				client, client->name);
		return;
	}
	d = &buf->buffer->datas[0];

	offset = SPA_MIN(d->chunk->offset, d->maxsize);
	size = SPA_MIN(d->chunk->size, d->maxsize - offset);

	while (size > 0) {
		res = send(client->source->fd,
				SPA_PTROFF(d->data, offset, void),
				size,
				MSG_NOSIGNAL | MSG_DONTWAIT);
		if (res < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				pw_log_warn("%p: client:%p [%s] send error %d: %m", impl,
						client, client->name, res);
				client_cleanup(client);
			}
			break;
		}
		offset += res;
		size -= res;
	}
	pw_stream_queue_buffer(client->capture, buf);
}

static void playback_process(void *data)
{
	struct client *client = data;
	struct impl *impl = client->impl;
	struct pw_buffer *buf;
	uint32_t size, offset;
	struct spa_data *d;
	int res;

	if ((buf = pw_stream_dequeue_buffer(client->playback)) == NULL) {
		pw_log_debug("%p: client:%p [%s] out of playback buffers: %m", impl,
				client, client->name);
		return;
	}
	d = &buf->buffer->datas[0];

	size = d->maxsize;
	if (buf->requested)
		size = SPA_MIN(size, buf->requested * impl->playback_frame_size);

	offset = 0;
	while (size > 0) {
		res = recv(client->source->fd,
				SPA_PTROFF(d->data, offset, void),
				size,
				MSG_DONTWAIT);
		if (res == 0) {
			pw_log_info("%p: client:%p [%s] disconnect", impl,
					client, client->name);
			client_cleanup(client);
			break;
		}
		if (res < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				pw_log_warn("%p: client:%p [%s] recv error %d: %m",
						impl, client, client->name, res);
			break;
		}
		offset += res;
		size -= res;
	}
	d->chunk->offset = 0;
	d->chunk->size = offset;
	d->chunk->stride = impl->playback_frame_size;

	pw_stream_queue_buffer(client->playback, buf);
}

static void capture_destroy(void *data)
{
	struct client *client = data;
	spa_hook_remove(&client->capture_listener);
	client->capture = NULL;
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                enum pw_stream_state state, const char *error)
{
	struct client *client = data;
	struct impl *impl = client->impl;

	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		if (!client->disconnect) {
			pw_log_info("%p: client:%p [%s] stream error %s",
					impl, client, client->name,
					pw_stream_state_as_string(state));
			client_cleanup(client);
		}
		break;
	default:
		break;
	}
}

static void playback_destroy(void *data)
{
	struct client *client = data;
	spa_hook_remove(&client->playback_listener);
	client->playback = NULL;
}

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.state_changed = on_stream_state_changed,
	.process = capture_process
};

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.state_changed = on_stream_state_changed,
	.process = playback_process
};

static int create_streams(struct impl *impl, struct client *client)
{
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct pw_properties *props;
	int res;

	if (impl->capture) {
		if ((props = pw_properties_copy(impl->capture_props)) == NULL)
			return -errno;

		pw_properties_setf(props,
				PW_KEY_MEDIA_NAME, "%s capture", client->name);
		client->capture = pw_stream_new(client->core,
				pw_properties_get(props, PW_KEY_MEDIA_NAME),
				props);
		if (client->capture == NULL)
			return -errno;

		pw_stream_add_listener(client->capture, &client->capture_listener,
				&capture_stream_events, client);
	}
	if (impl->playback) {
		props = pw_properties_copy(impl->playback_props);
		if (props == NULL)
			return -errno;

		pw_properties_setf(props,
				PW_KEY_MEDIA_NAME, "%s playback", client->name);

		client->playback = pw_stream_new(client->core,
				pw_properties_get(props, PW_KEY_MEDIA_NAME),
				props);
		if (client->playback == NULL)
			return -errno;

		pw_stream_add_listener(client->playback, &client->playback_listener,
				&playback_stream_events, client);
	}


	if (impl->capture) {
		n_params = 0;
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
				&impl->capture_info);

		if ((res = pw_stream_connect(client->capture,
				PW_DIRECTION_INPUT,
				PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_RT_PROCESS,
				params, n_params)) < 0)
			return res;
	}
	if (impl->playback) {
		n_params = 0;
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
				&impl->playback_info);

		if ((res = pw_stream_connect(client->playback,
				PW_DIRECTION_OUTPUT,
				PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_RT_PROCESS,
				params, n_params)) < 0)
			return res;
	}
	return 0;
}

static void on_core_proxy_destroy(void *data)
{
	struct client *client = data;
	spa_hook_remove(&client->core_proxy_listener);
	client->core = NULL;
	client_cleanup(client);
}

static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_CORE_EVENTS,
	.destroy = on_core_proxy_destroy,
};

static void
on_connect(void *data, int fd, uint32_t mask)
{
	struct server *server = data;
	struct impl *impl = server->impl;
	struct sockaddr_in addr;
	socklen_t addrlen;
	int client_fd, val;
	struct client *client = NULL;
	struct pw_properties *props = NULL;

	addrlen = sizeof(addr);
	client_fd = accept4(fd, (struct sockaddr *) &addr, &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (client_fd < 0)
		goto error;

	if (server->n_clients >= MAX_CLIENTS) {
		close(client_fd);
		errno = ECONNREFUSED;
		goto error;
	}

	client = calloc(1, sizeof(struct client));
	if (client == NULL)
		goto error;

	client->impl = impl;
	client->server = server;
	spa_list_append(&server->client_list, &client->link);
	server->n_clients++;

	if (inet_ntop(addr.sin_family, &addr.sin_addr.s_addr, client->name, sizeof(client->name)) == NULL)
		snprintf(client->name, sizeof(client->name), "client %d", client_fd);

	client->source = pw_loop_add_io(impl->loop,
					client_fd,
					SPA_IO_ERR | SPA_IO_HUP,
					true, on_client_data, client);
	if (client->source == NULL)
		goto error;

	pw_log_info("%p: client:%p [%s] connected", impl, client, client->name);

	props = pw_properties_new(
			PW_KEY_CLIENT_API, "protocol-simple",
			PW_KEY_REMOTE_NAME,
				pw_properties_get(impl->props, PW_KEY_REMOTE_NAME),
			NULL);
	if (props == NULL)
		goto error;

	pw_properties_setf(props,
			"protocol.server.type", "%s",
			server->type == SERVER_TYPE_TCP ? "tcp" : "unix");

	if (server->type == SERVER_TYPE_UNIX) {
		goto error;
	} else if (server->type == SERVER_TYPE_TCP) {
		val = 1;
		if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
					(const void *) &val, sizeof(val)) < 0)
	            pw_log_warn("TCP_NODELAY failed: %m");

		val = IPTOS_LOWDELAY;
		if (setsockopt(client_fd, IPPROTO_IP, IP_TOS,
					(const void *) &val, sizeof(val)) < 0)
	            pw_log_warn("IP_TOS failed: %m");

		pw_properties_set(props, PW_KEY_CLIENT_ACCESS, "restricted");
	}

	client->core = pw_context_connect(impl->context, props, 0);
	props = NULL;
	if (client->core == NULL)
		goto error;

	pw_proxy_add_listener((struct pw_proxy*)client->core,
			&client->core_proxy_listener, &core_proxy_events,
			client);

	create_streams(impl, client);

	return;
error:
	pw_log_error("%p: failed to create client: %m", impl);
	pw_properties_free(props);
	if (client != NULL)
		client_free(client);
	return;
}

static int make_tcp_socket(struct server *server, const char *name, const char *ifname,
		const char *ifaddress)
{
	struct sockaddr_storage addr;
	int res, on;
	socklen_t len = 0;
	spa_autoclose int fd = -1;

	if ((res = pw_net_parse_address_port(name, ifaddress, DEFAULT_PORT, &addr, &len)) < 0) {
		pw_log_error("%p: can't parse address %s: %s", server,
				name, spa_strerror(res));
		goto error;
	}

	if ((fd = socket(addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		res = -errno;
		pw_log_error("%p: socket() failed: %m", server);
		goto error;
	}
#ifdef SO_BINDTODEVICE
	if (ifname && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0) {
		res = -errno;
		pw_log_error("%p: setsockopt(SO_BINDTODEVICE) failed: %m", server);
		goto error;
	}
#endif
	on = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *) &on, sizeof(on)) < 0)
		pw_log_warn("%p: setsockopt(): %m", server);

	if (bind(fd, (struct sockaddr *) &addr, len) < 0) {
		res = -errno;
		pw_log_error("%p: bind() failed: %m", server);
		goto error;
	}
	if (listen(fd, 5) < 0) {
		res = -errno;
		pw_log_error("%p: listen() failed: %m", server);
		goto error;
	}
	if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
		res = -errno;
		pw_log_error("%p: getsockname() failed: %m", server);
		goto error;
	}

	server->type = SERVER_TYPE_TCP;
	server->addr = addr;

	return spa_steal_fd(fd);

error:
	return res;
}

static void server_free(struct server *server)
{
	struct impl *impl = server->impl;
	struct client *c;

	pw_log_debug("%p: free server %p", impl, server);

	spa_list_remove(&server->link);
	spa_list_consume(c, &server->client_list, link)
		client_free(c);
	if (server->source)
		pw_loop_destroy_source(impl->loop, server->source);
	free(server);
}

static struct server *create_server(struct impl *impl, const char *address)
{
	int fd, res;
	struct server *server;

	server = calloc(1, sizeof(struct server));
	if (server == NULL)
		return NULL;

	server->impl = impl;
	spa_list_init(&server->client_list);
	spa_list_append(&impl->server_list, &server->link);

	if (spa_strstartswith(address, "tcp:")) {
		fd = make_tcp_socket(server, address+4, impl->ifname, impl->ifaddress);
	} else {
		pw_log_error("address %s does not start with tcp:", address);
		fd = -EINVAL;
	}
	if (fd < 0) {
		res = fd;
		goto error;
	}
	server->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, true, on_connect, server);
	if (server->source == NULL) {
		res = -errno;
		pw_log_error("%p: can't create server source: %m", impl);
		goto error_close;
	}
	return server;

error_close:
	close(fd);
error:
	server_free(server);
	errno = -res;
	return NULL;
}

static void impl_free(struct impl *impl)
{
	struct server *s;

	spa_hook_remove(&impl->module_listener);
	spa_list_consume(s, &impl->server_list, link)
		server_free(s);
	pw_properties_free(impl->capture_props);
	pw_properties_free(impl->playback_props);
	pw_properties_free(impl->props);
	free(impl->ifname);
	free(impl->ifaddress);
	free(impl);
}

static int calc_frame_size(struct spa_audio_info_raw *info)
{
	int res = info->channels;
	switch (info->format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_ALAW:
	case SPA_AUDIO_FORMAT_ULAW:
		return res;
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
	case SPA_AUDIO_FORMAT_U16:
		return res * 2;
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
	case SPA_AUDIO_FORMAT_U24:
		return res * 3;
	case SPA_AUDIO_FORMAT_S24_32:
	case SPA_AUDIO_FORMAT_S24_32_OE:
	case SPA_AUDIO_FORMAT_S32:
	case SPA_AUDIO_FORMAT_S32_OE:
	case SPA_AUDIO_FORMAT_U32:
	case SPA_AUDIO_FORMAT_U32_OE:
	case SPA_AUDIO_FORMAT_F32:
	case SPA_AUDIO_FORMAT_F32_OE:
		return res * 4;
	case SPA_AUDIO_FORMAT_F64:
	case SPA_AUDIO_FORMAT_F64_OE:
		return res * 8;
	default:
		return -ENOTSUP;
	}
}

static int parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	int res;
	if ((res = spa_audio_info_raw_init_dict_keys(info,
			&SPA_DICT_ITEMS(
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT, DEFAULT_FORMAT),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE, SPA_STRINGIFY(DEFAULT_RATE)),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_POSITION, DEFAULT_POSITION)),
			&props->dict,
			SPA_KEY_AUDIO_FORMAT,
			SPA_KEY_AUDIO_RATE,
			SPA_KEY_AUDIO_CHANNELS,
			SPA_KEY_AUDIO_POSITION, NULL)) < 0)
		return res;

	return calc_frame_size(info);
}

static void copy_props(struct impl *impl, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(impl->props, key)) != NULL) {
		if (pw_properties_get(impl->capture_props, key) == NULL)
			pw_properties_set(impl->capture_props, key, str);
		if (pw_properties_get(impl->playback_props, key) == NULL)
			pw_properties_set(impl->playback_props, key, str);
	}
}

static int parse_params(struct impl *impl)
{
	const char *str;
	struct spa_json it[1];
	char value[512];
	int res;

	pw_properties_fetch_bool(impl->props, "capture", &impl->capture);
	pw_properties_fetch_bool(impl->props, "playback", &impl->playback);
	if (!impl->playback && !impl->capture) {
		pw_log_error("missing capture or playback param");
		return -EINVAL;
	}

	if (pw_properties_get(impl->props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(impl->props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(impl->props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(impl->props, PW_KEY_NODE_NETWORK, "true");

	impl->capture_props = pw_properties_new(
			PW_KEY_TARGET_OBJECT, pw_properties_get(impl->props, "capture.node"),
			PW_KEY_STREAM_CAPTURE_SINK, pw_properties_get(impl->props,
				PW_KEY_STREAM_CAPTURE_SINK),
			NULL);
	impl->playback_props = pw_properties_new(
			PW_KEY_TARGET_OBJECT, pw_properties_get(impl->props, "playback.node"),
			NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL) {
		pw_log_error("can't create props: %m");
		return -errno;
	}

	if ((str = pw_properties_get(impl->props, "capture.props")) != NULL)
		pw_properties_update_string(impl->capture_props, str, strlen(str));
	if ((str = pw_properties_get(impl->props, "playback.props")) != NULL)
		pw_properties_update_string(impl->playback_props, str, strlen(str));

	copy_props(impl, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, PW_KEY_AUDIO_RATE);
	copy_props(impl, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, PW_KEY_NODE_RATE);
	copy_props(impl, PW_KEY_NODE_NAME);
	copy_props(impl, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, PW_KEY_NODE_GROUP);
	copy_props(impl, PW_KEY_NODE_LATENCY);
	copy_props(impl, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, PW_KEY_NODE_NETWORK);

	if ((res = parse_audio_info(impl->capture_props, &impl->capture_info)) <= 0) {
		pw_log_error("unsupported capture audio format:%d channels:%d",
				impl->capture_info.format, impl->capture_info.channels);
		return -EINVAL;
	}
	impl->capture_frame_size = res;

	if ((res = parse_audio_info(impl->playback_props, &impl->playback_info)) <= 0) {
		pw_log_error("unsupported playback audio format:%d channels:%d",
				impl->playback_info.format, impl->playback_info.channels);
		return -EINVAL;
	}
	impl->playback_frame_size = res;

	if (impl->capture_info.rate != 0 &&
	    pw_properties_get(impl->capture_props, PW_KEY_NODE_RATE) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_RATE,
				"1/%u", impl->capture_info.rate);
	if (impl->playback_info.rate != 0 &&
	    pw_properties_get(impl->playback_props, PW_KEY_NODE_RATE) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_RATE,
				"1/%u", impl->playback_info.rate);

	str = pw_properties_get(impl->props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;
	str = pw_properties_get(impl->props, "local.ifaddress");
	impl->ifaddress = str ? strdup(str) : NULL;

	if ((str = pw_properties_get(impl->props, "server.address")) == NULL)
		str = DEFAULT_SERVER;

        if (spa_json_begin_array_relax(&it[0], str, strlen(str)) > 0) {
                while (spa_json_get_string(&it[0], value, sizeof(value)) > 0) {
                        if (create_server(impl, value) == NULL) {
				pw_log_warn("%p: can't create server for %s: %m",
					impl, value);
			}
		}
	}
	return 0;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	pw_log_debug("module %p: destroy", impl);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	struct server *s;
	FILE *f;
	char *str;
	size_t size;
	int res;
	struct spa_dict_item it[1];

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = pw_properties_new(NULL, NULL);

	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);
	impl->props = props;
	spa_list_init(&impl->server_list);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	impl->work_queue = pw_context_get_work_queue(context);

	if ((res = parse_params(impl)) < 0)
		goto error_free;

	if ((f = open_memstream(&str, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		goto error_free;
	}

	fprintf(f, "[");

	spa_list_for_each(s, &impl->server_list, link) {
		char ip[128];
		uint16_t port = 0;
		bool ipv4;

		if (pw_net_get_ip(&s->addr, ip, sizeof(ip), &ipv4, &port) < 0)
			continue;

		fprintf(f, " \"%s%s%s:%d\"", ipv4 ? "" : "[", ip, ipv4 ? "" : "]", port);
	}
	fprintf(f, " ]");
	fclose(f);

	pw_log_info("listening on %s", str);
	it[0] = SPA_DICT_ITEM_INIT("server.address", str);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(it));

	free(str);

	return 0;

error_free:
	impl_free(impl);
	return res;
}
