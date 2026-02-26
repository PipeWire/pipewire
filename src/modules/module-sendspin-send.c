/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans <wim.taymans@proton.me> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ctype.h>

#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/defs.h>
#include <spa/utils/dll.h>
#include <spa/utils/json.h>
#include <spa/utils/json-builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/control/control.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include "module-sendspin/sendspin.h"
#include "module-sendspin/websocket.h"
#include "module-sendspin/zeroconf.h"
#include "network-utils.h"

/** \page page_module_sendspin_send sendspin sender
 *
 * The `sendspin-send` module creates a PipeWire sink that sends audio
 * packets using the sendspin protocol to a client.
 *
 * The sender will listen on a specific port (8927) and create a stream for
 * each connection.
 *
 * In combination with a virtual sink, each of the client streams can be sent
 * the same data in the client specific format.
 *
 * ## Module Name
 *
 * `libpipewire-module-sendspin-send`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `local.ifname = <str>`: interface name to use
 * - `local.ifaddress = <str>`: interface address to use
 * - `source.ip = <str>`: the source ip address to listen on, default "127.0.0.1"
 * - `source.port = <int>`: the source port to listen on, default 8927
 * - `source.path = <str>`: comma separated list of paths to listen on,
 *                  default "/sendspin"
 * - `sendspin.ip`: an array of IP addresses of sendspin clients to connect to
 * - `sendspin.port`: the port of the sendspin client to connect to, default 8928
 * - `sendspin.path`: the path of the sendspin client to connect to, default "/sendspin"
 * - `sendspin.group-id`: the group-id of the server, default random
 * - `sendspin.group-name`: the group-name of the server, default "PipeWire"
 * - `sendspin.delay`: the delay to add to clients in seconds. Default 5.0
 * - `node.always-process = <bool>`: true to send silence even when not connected.
 * - `stream.props = {}`: properties to be passed to all the stream
 * - `stream.rules` = <rules>: match rules, use the create-stream action to
 *                    make a stream for the client.
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref SPA_KEY_AUDIO_LAYOUT
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_MEDIA_NAME
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_VIRTUAL
 *
 * ## Example configuration
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-sendspin-send.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-sendspin-send
 *     args = {
 *         #local.ifname = eth0
 *         #source.ip = 127.0.0.1
 *         #source.port = 8927
 *         #source.path = "/sendspin"
 *         #sendspin.ip = [ 127.0.0.1 ]
 *         #sendspin.port = 8928
 *         #sendspin.path = "/sendspin"
 *         #sendspin.group-id = "abcded"
 *         #sendspin.group-name = "PipeWire"
 *         #sendspin.delay = 5.0
 *         #node.always-process = false
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *            #media.class = "Audio/sink"
 *            #node.name = "sendspin-send"
 *         }
 *         stream.rules = [
 *             {   matches = [
 *                     {    sendspin.ip = "~.*"
 *                          #sendspin.port = 8928
 *                          #sendspin.path = "/sendspin"
 *                          #zeroconf.ifindex = 0
 *                          #zeroconf.name = ""
 *                          #zeroconf.type = "_sendspin._tcp"
 *                          #zeroconf.domain = "local"
 *                          #zeroconf.hostname = ""
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         stream.props = {
 *                             #target.object = ""
 *                             #media.class = "Audio/Sink"
 *                         }
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * \since 1.6.0
 */

#define NAME "sendspin-send"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_SOURCE_IP	"127.0.0.1"
#define DEFAULT_SOURCE_PORT	PW_SENDSPIN_DEFAULT_SERVER_PORT
#define DEFAULT_SOURCE_PATH	PW_SENDSPIN_DEFAULT_PATH

#define DEFAULT_CLIENT_PORT	PW_SENDSPIN_DEFAULT_CLIENT_PORT
#define DEFAULT_SENDSPIN_PATH	PW_SENDSPIN_DEFAULT_PATH

#define DEFAULT_SENDSPIN_DELAY	5.0

#define DEFAULT_POSITION	"[ FL FR ]"

#define DEFAULT_CREATE_RULES	\
        "[ { matches = [ { sendspin.ip = \"~.*\" } ] actions = { create-stream = { } } } ] "

#define USAGE   "( local.ifname=<local interface name to use> ) "						\
		"( source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> ) "				\
 		"( source.port=<int, source port, default:"SPA_STRINGIFY(DEFAULT_SOURCE_PORT)"> "		\
		"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "					\
		"( stream.props= { key=value ... } ) "

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@proton.me>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Sendspin sender" },
	{ PW_KEY_MODULE_USAGE,	USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct client {
	struct impl *impl;
	struct spa_list link;

	char *name;
	struct pw_properties *props;

	struct pw_websocket_connection *conn;
	struct spa_hook conn_listener;

	struct spa_audio_info info;
	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_io_position *io_position;
	struct pw_timer timer;

	uint64_t delay_usec;
	uint32_t stride;

	int buffer_capacity;
#define ROLE_PLAYER	(1<<0)
#define ROLE_METADATA	(1<<1)
	uint32_t supported_roles;
#define COMMAND_VOLUME	(1<<0)
#define COMMAND_MUTE	(1<<1)
	uint32_t supported_commands;

	bool playing;
};

struct impl {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *context;

	struct pw_loop *main_loop;
	struct pw_loop *data_loop;
	struct pw_timer_queue *timer_queue;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	unsigned int do_disconnect:1;

	struct pw_zeroconf *zeroconf;
	struct spa_hook zeroconf_listener;

	float delay;
	bool always_process;

	struct pw_properties *stream_props;

	struct pw_websocket *websocket;
	struct spa_hook websocket_listener;

	struct spa_list clients;

};

static int send_group_update(struct client *c, bool playing);
static int send_stream_start(struct client *c);
static int send_server_state(struct client *c);

static void on_stream_destroy(void *d)
{
	struct client *c = d;
	spa_hook_remove(&c->stream_listener);
	c->stream = NULL;
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct client *c = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		//pw_impl_module_schedule_destroy(c->impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
		send_group_update(c, false);
		break;
	case PW_STREAM_STATE_STREAMING:
		send_group_update(c, true);
		break;
	default:
		break;
	}
}

static uint64_t get_time_us(struct client *c)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return SPA_TIMESPEC_TO_USEC(&now);
}

static void on_playback_stream_process(void *d)
{
	struct client *c = d;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *p;
	struct iovec iov[2];
	uint8_t header[9];
	uint64_t timestamp;

	if ((b = pw_stream_dequeue_buffer(c->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	if (c->playing) {
		buf = b->buffer;
		if ((p = buf->datas[0].data) == NULL)
			return;

		timestamp = c->io_position ?
			c->io_position->clock.nsec / 1000 :
			get_time_us(c);
		timestamp += c->delay_usec;

		header[0] = 4;
		header[1] = (timestamp >> 56) & 0xff;
		header[2] = (timestamp >> 48) & 0xff;
		header[3] = (timestamp >> 40) & 0xff;
		header[4] = (timestamp >> 32) & 0xff;
		header[5] = (timestamp >> 24) & 0xff;
		header[6] = (timestamp >> 16) & 0xff;
		header[7] = (timestamp >>  8) & 0xff;
		header[8] = (timestamp      ) & 0xff;

		iov[0].iov_base = header;
		iov[0].iov_len = sizeof(header);
		iov[1].iov_base = p;
		iov[1].iov_len = buf->datas[0].chunk->size;

		pw_websocket_connection_send(c->conn,
				PW_WEBSOCKET_OPCODE_BINARY, iov, 2);
	}
	pw_stream_queue_buffer(c->stream, b);
}

static void
on_stream_param_changed(void *d, uint32_t id, const struct spa_pod *param)
{
	struct client *c = d;

	if (param == NULL)
		return;

	switch (id) {
	case SPA_PARAM_Format:
		if (spa_format_audio_parse(param, &c->info) < 0)
			return;
		send_stream_start(c);
		break;
	case SPA_PARAM_Tag:
		send_server_state(c);
		break;
	}
}

static void on_stream_io_changed(void *d, uint32_t id, void *area, uint32_t size)
{
	struct client *c = d;
	switch (id) {
	case SPA_IO_Position:
		c->io_position = area;
		break;
	}
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.io_changed = on_stream_io_changed,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.process = on_playback_stream_process
};

static int create_stream(struct client *c)
{
	struct impl *impl = c->impl;
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const char *client_id, *ip, *port, *client_name;
	struct pw_properties *props = pw_properties_copy(c->props);

	ip = pw_properties_get(props, "sendspin.ip");
	port = pw_properties_get(props, "sendspin.port");
	client_id = pw_properties_get(props, "sendspin.client-id");
	client_name = pw_properties_get(props, "sendspin.client-name");

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "sendspin.%s.%s.%s", client_id, ip, port);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "Sendspin to %s", client_name);
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "Sendspin to %s", client_name);


	c->stream = pw_stream_new(impl->core, "sendspin sender", props);
	if (c->stream == NULL)
		return -errno;

	pw_stream_add_listener(c->stream,
			&c->stream_listener,
			&playback_stream_events, c);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_build(&b,
			SPA_PARAM_EnumFormat, &c->info);

	if ((res = pw_stream_connect(c->stream,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int send_server_hello(struct client *c)
{
	struct impl *impl = c->impl;
	struct spa_json_builder b;
	int res;
	size_t size;
	char *mem;

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,   "{");
	spa_json_builder_object_string(&b,  "type", "server/hello");
	spa_json_builder_object_push(&b,    "payload", "{");
	spa_json_builder_object_string(&b,    "server_id", pw_properties_get(impl->props, "sendspin.server-id"));
	spa_json_builder_object_string(&b,    "name", pw_properties_get(impl->props, "sendspin.server-name"));
	spa_json_builder_object_int(&b,       "version", 1);
	spa_json_builder_object_push(&b,      "active_roles", "[");
	if (c->supported_roles & ROLE_PLAYER)
		spa_json_builder_array_string(&b, "player@v1");
	if (c->supported_roles & ROLE_METADATA)
		spa_json_builder_array_string(&b, "metadata@v1");
	spa_json_builder_pop(&b,              "]");
	spa_json_builder_object_string(&b,    "connection_reason", "discovery");
	spa_json_builder_pop(&b,            "}");
	spa_json_builder_pop(&b,          "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(c->conn, mem, size);
	free(mem);

	return res;
}

static int send_server_state(struct client *c)
{
	struct spa_json_builder b;
	int res;
	size_t size;
	char *mem;

	if (!SPA_FLAG_IS_SET(c->supported_roles, ROLE_METADATA))
		return 0;

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "server/state");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_push(&b,     "metadata", "{");
	spa_json_builder_object_uint(&b,       "timestamp", get_time_us(c));
	spa_json_builder_pop(&b,             "}");
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(c->conn, mem, size);
	free(mem);
	return res;
}

static int send_server_time(struct client *c, uint64_t t1, uint64_t t2)
{
	struct spa_json_builder b;
	int res;
	uint64_t t3;
	size_t size;
	char *mem;

	t3 = get_time_us(c);

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "server/time");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_uint(&b,     "client_transmitted", t1);
	spa_json_builder_object_uint(&b,     "server_received", t2);
	spa_json_builder_object_uint(&b,     "server_transmitted", t3);
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(c->conn, mem, size);
	free(mem);
	return res;
}

#if 0
static int send_server_command(struct client *c)
{
	return 0;
}
#endif

static int send_stream_start(struct client *c)
{
	struct spa_json_builder b;
	int res, channels, rate, depth = 0;
	const char *codec;
	size_t size;
	char *mem;

	switch (c->info.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		codec = "pcm";
		channels = c->info.info.raw.channels;
		rate = c->info.info.raw.rate;
		switch (c->info.info.raw.format) {
		case SPA_AUDIO_FORMAT_S16_LE:
			depth = 16;
			break;
		case SPA_AUDIO_FORMAT_S24_LE:
			depth = 24;
			break;
		default:
			return -ENOTSUP;
		}
		break;
	case SPA_MEDIA_SUBTYPE_opus:
		codec = "opus";
		channels = c->info.info.opus.channels;
		rate = c->info.info.opus.rate;
		break;
	case SPA_MEDIA_SUBTYPE_flac:
		codec = "flac";
		channels = c->info.info.flac.channels;
		rate = c->info.info.flac.rate;
		break;
	default:
		return -ENOTSUP;
	}

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,   "{");
	spa_json_builder_object_string(&b,  "type", "stream/start");
	spa_json_builder_object_push(&b,    "payload", "{");
	spa_json_builder_object_push(&b,      "player", "{");
	spa_json_builder_object_string(&b,      "codec", codec);
	spa_json_builder_object_int(&b,         "channels", channels);
	spa_json_builder_object_int(&b,         "sample_rate", rate);
	if (depth)
		spa_json_builder_object_int(&b, "bit_depth", depth);
	spa_json_builder_pop(&b,              "}");
	spa_json_builder_pop(&b,            "}");
	spa_json_builder_pop(&b,          "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(c->conn, mem, size);
	free(mem);
	return res;
}

#if 0
static int send_stream_end(struct client *c)
{
	struct spa_json_builder b;
	int res;
	size_t size;
	char *mem;

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "stream/end");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_push(&b,     "roles", "[");
	spa_json_builder_array_string(&b,      "player");
	spa_json_builder_array_string(&b,      "metadata");
	spa_json_builder_pop(&b,             "]");
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(c->conn, mem, size);
	free(mem);
	return res;
}
#endif

static int send_group_update(struct client *c, bool playing)
{
	struct impl *impl = c->impl;
	struct spa_json_builder b;
	int res;
	char *mem;
	size_t size;

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "group/update");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_string(&b,   "playback_state", playing ? "playing" : "stopped");
	spa_json_builder_object_string(&b,   "group_id", pw_properties_get(impl->props, "sendspin.group-id"));
	spa_json_builder_object_string(&b,   "group_name", pw_properties_get(impl->props, "sendspin.group-name"));
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	c->playing = playing;

	res = pw_websocket_connection_send_text(c->conn, mem, size);
	free(mem);
	return res;
}

/* {"codec":"pcm","sample_rate":44100,"channels":2,"bit_depth":16} */
static int parse_codec(struct client *c, struct spa_json *object, struct spa_audio_info *info)
{
	char key[256], codec[64] = "";
        const char *v;
	int l, sample_rate = 0, channels = 0, bit_depth = 0;

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	while ((l = spa_json_object_next(object, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "codec")) {
			if (spa_json_parse_stringn(v, l, codec, sizeof(codec)) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "sample_rate")) {
			if (spa_json_parse_int(v, l, &sample_rate) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "channels")) {
			if (spa_json_parse_int(v, l, &channels) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "bit_depth")) {
			if (spa_json_parse_int(v, l, &bit_depth) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "codec_header")) {
		}
	}
	if (sample_rate == 0 || channels == 0)
		return -EINVAL;

	if (spa_streq(codec, "pcm")) {
		info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
		info->info.raw.rate = sample_rate;
		info->info.raw.channels = channels;
		switch (bit_depth) {
		case 16:
			info->info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
			break;
		case 24:
			info->info.raw.format = SPA_AUDIO_FORMAT_S24_LE;
			break;
		default:
			return -EINVAL;
		}
	}
	else if (spa_streq(codec, "opus")) {
		info->media_subtype = SPA_MEDIA_SUBTYPE_opus;
		info->info.opus.rate = sample_rate;
		info->info.opus.channels = channels;
	}
	else if (spa_streq(codec, "flac")) {
		info->media_subtype = SPA_MEDIA_SUBTYPE_flac;
		info->info.flac.rate = sample_rate;
		info->info.flac.channels = channels;
	}
	else
		return -EINVAL;

	return 0;
}

static int parse_player_v1_support(struct client *c, struct spa_json *payload)
{
	struct spa_json it[2];
	char key[256], *t;
        const char *v;
	int l, res;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "supported_formats")) {
			int count = 0;

			if (!spa_json_is_array(v, l))
				return -EPROTO;
			spa_json_enter(payload, &it[0]);

			while ((l = spa_json_next(&it[0], &v)) > 0) {
				struct spa_audio_info info;
				if (!spa_json_is_object(v, l))
					return -EPROTO;

				spa_json_enter(&it[0], &it[1]);
				if ((res = parse_codec(c, &it[1], &info)) < 0)
					return res;

				if (count++ == 0)
					c->info = info;
			}
		}
		else if (spa_streq(key, "buffer_capacity")) {
			if (spa_json_parse_int(v, l, &c->buffer_capacity) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "supported_commands")) {
			if (!spa_json_is_array(v, l))
				return -EPROTO;
			spa_json_enter(payload, &it[0]);

			while ((l = spa_json_next(&it[0], &v)) > 0) {
				t = alloca(l+1);
				spa_json_parse_stringn(v, l, t, l+1);
				if (spa_streq(t, "volume"))
					c->supported_commands |= COMMAND_VOLUME;
				else if (spa_streq(t, "mute"))
					c->supported_commands |= COMMAND_MUTE;
			}
		}
	}
	return 0;
}

static int handle_client_hello(struct client *c, struct spa_json *payload)
{
	struct spa_json it[1];
	char key[256], *t;
        const char *v;
	int res, l, version = 0;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "client_id")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);
			pw_properties_set(c->props, "sendspin.client-id", t);
		}
		else if (spa_streq(key, "name")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);
			pw_properties_set(c->props, "sendspin.client-name", t);
		}
		else if (spa_streq(key, "version")) {
			if (spa_json_parse_int(v, l, &version) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "supported_roles")) {
			if (!spa_json_is_array(v, l))
				return -EPROTO;

			spa_json_enter(payload, &it[0]);
			while ((l = spa_json_next(&it[0], &v)) > 0) {
				t = alloca(l+1);
				spa_json_parse_stringn(v, l, t, l+1);

				if (spa_streq(t, "player@v1"))
					c->supported_roles |= ROLE_PLAYER;
				else if (spa_streq(t, "metadata@v1"))
					c->supported_roles |= ROLE_METADATA;
			}
		}
		else if (spa_streq(key, "player_support") ||
		    spa_streq(key, "player@v1_support")) {
			if (!spa_json_is_object(v, l))
				return -EPROTO;
			spa_json_enter(payload, &it[0]);
			if ((res = parse_player_v1_support(c, &it[0])) < 0)
				return res;
		}
	}
	if (version != 1)
		return -ENOTSUP;

	return send_server_hello(c);
}

static int handle_client_state(struct client *c, struct spa_json *payload)
{
	struct spa_json it[1];
	char key[256];
        const char *v;
	int l;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "player")) {
			if (!spa_json_is_object(v, l))
				return -EPROTO;
			spa_json_enter(payload, &it[0]);
			while ((l = spa_json_object_next(&it[0], key, sizeof(key), &v)) > 0) {
				if (spa_streq(key, "state")) {
				}
				else if (spa_streq(key, "volume")) {
				}
				else if (spa_streq(key, "mute")) {
				}
			}
		}
	}
	if (c->stream == NULL)
		create_stream(c);
	return 0;
}

static int parse_uint64(const char *val, int len, uint64_t *result)
{
        char buf[64];
        char *end;

        if (len <= 0 || len >= (int)sizeof(buf))
                return 0;

        memcpy(buf, val, len);
        buf[len] = '\0';

        *result = strtoull(buf, &end, 0);
        return len > 0 && end == buf + len;
}

static int handle_client_time(struct client *c, struct spa_json *payload)
{
	char key[256];
        const char *v;
	int l;
	uint64_t t1 = 0,t2;

	t2 = get_time_us(c);

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "client_transmitted")) {
			if (parse_uint64(v, l, &t1) <= 0)
                                return -EINVAL;
		}
	}
	if (t1 == 0)
		return -EPROTO;

	return send_server_time(c, t1, t2);
}

static int handle_client_command(struct client *c, struct spa_json *payload)
{
	return 0;
}

/* {"player":{}} */
static int handle_stream_request_format(struct client *c, struct spa_json *payload)
{
	struct spa_json it[1];
	char key[256];
        const char *v;
	int l;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "player")) {
			if (!spa_json_is_object(v, l))
				return -EPROTO;
			spa_json_enter(payload, &it[0]);
			parse_codec(c, &it[0], &c->info);
		}
	}
	return 0;
}

static int handle_client_goodbye(struct client *c, struct spa_json *payload)
{
	if (c->stream != NULL) {
		pw_stream_destroy(c->stream);
		c->stream = NULL;
	}
	return 0;
}

/* { "type":... "payload":{...} } */
static int do_parse_text(struct client *c, const char *content, int size)
{
	struct spa_json it[2], *payload = NULL;
	char key[256], type[256] = "";
        const char *v;
	int res, l;

	pw_log_info("received text %.*s", size, content);

	if (spa_json_begin_object(&it[0], content, size) <= 0)
		return -EINVAL;

	while ((l = spa_json_object_next(&it[0], key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "payload")) {
			if (!spa_json_is_object(v, l))
				return -EPROTO;

			spa_json_enter(&it[0], &it[1]);
			payload = &it[1];
		}
		else if (spa_streq(key, "type")) {
			if (spa_json_parse_stringn(v, l, type, sizeof(type)) <= 0)
				continue;
		}
	}
	if (spa_streq(type, "client/hello"))
		res = handle_client_hello(c, payload);
	else if (spa_streq(type, "client/state"))
		res = handle_client_state(c, payload);
	else if (spa_streq(type, "client/time"))
		res = handle_client_time(c, payload);
	else if (spa_streq(type, "client/command"))
		res = handle_client_command(c, payload);
	else if (spa_streq(type, "client/goodbye"))
		res = handle_client_goodbye(c, payload);
	else if (spa_streq(type, "stream/request-format"))
		res = handle_stream_request_format(c, payload);
	else
		res = 0;

	return res;
}

static void on_connection_message(void *data, int opcode, void *payload, size_t size)
{
	struct client *c = data;
	if (opcode == PW_WEBSOCKET_OPCODE_TEXT) {
		do_parse_text(c, payload, size);
	} else {
		pw_log_warn("%02x unknown %08x", opcode, (int)size);
	}
}

static void client_free(struct client *c)
{
	struct impl *impl = c->impl;

	spa_list_remove(&c->link);

	handle_client_goodbye(c, NULL);
	if (c->conn) {
		spa_hook_remove(&c->conn_listener);
		pw_websocket_connection_destroy(c->conn);
	} else {
		pw_websocket_cancel(impl->websocket, c);
	}
	pw_timer_queue_cancel(&c->timer);
	pw_properties_free(c->props);
	free(c->name);
	free(c);
}

static void on_connection_destroy(void *data)
{
	struct client *c = data;
	c->conn = NULL;
	pw_log_info("connection %p destroy", c);
}

static void on_connection_error(void *data, int res, const char *reason)
{
	struct client *c = data;
	pw_log_error("connection %p error %d %s", c, res, reason);
}

static void on_connection_disconnected(void *data)
{
	struct client *c = data;
	client_free(c);
}

static const struct pw_websocket_connection_events websocket_connection_events = {
	PW_VERSION_WEBSOCKET_CONNECTION_EVENTS,
	.destroy = on_connection_destroy,
	.error = on_connection_error,
	.disconnected = on_connection_disconnected,
	.message = on_connection_message,
};

static struct client *client_new(struct impl *impl, const char *name, struct pw_properties *props)
{
	struct client *c;

	if ((c = calloc(1, sizeof(*c))) == NULL)
		goto error;

	c->impl = impl;
	spa_list_append(&impl->clients, &c->link);

	c->props = props;
	c->name = name ? strdup(name) : NULL;
	c->delay_usec = (uint64_t)(impl->delay * SPA_USEC_PER_SEC);

	return c;
error:
	pw_properties_free(props);
	return NULL;
}

static int client_connect(struct client *c)
{
	struct impl *impl = c->impl;
	const char *addr, *port, *path;
	addr = pw_properties_get(c->props, "sendspin.ip");
	port = pw_properties_get(c->props, "sendspin.port");
	path = pw_properties_get(c->props, "sendspin.path");
	return pw_websocket_connect(impl->websocket, c, addr, port, path);
}

static void client_connected(struct client *c, struct pw_websocket_connection *conn)
{
	if (c->conn) {
		spa_hook_remove(&c->conn_listener);
		pw_websocket_connection_destroy(c->conn);
	}
	c->conn = conn;
	if (conn)
		pw_websocket_connection_add_listener(c->conn, &c->conn_listener,
				&websocket_connection_events, c);
}

static struct client *client_find(struct impl *impl, const char *name)
{
	struct client *c;
	spa_list_for_each(c, &impl->clients, link) {
		if (spa_streq(c->name, name))
			return c;
	}
	return NULL;
}

struct match_info {
	struct impl *impl;
	const char *name;
	struct pw_properties *props;
	struct pw_websocket_connection *conn;
	bool matched;
};

static int rule_matched(void *data, const char *location, const char *action,
                        const char *str, size_t len)
{
	struct match_info *i = data;
	struct impl *impl = i->impl;
	int res = 0;

	i->matched = true;
	if (spa_streq(action, "create-stream")) {
		struct client *c;

		pw_properties_update_string(i->props, str, len);
		if ((c = client_new(impl, i->name, spa_steal_ptr(i->props))) == NULL)
			return -errno;
		if (i->conn)
			client_connected(c, i->conn);
		else
			client_connect(c);
	}
	return res;
}

static int match_client(struct impl *impl, const char *name, struct pw_properties *props,
		struct pw_websocket_connection *conn)
{
	const char *str;
	struct match_info minfo = {
		.impl = impl,
		.name = name,
		.props = props,
		.conn = conn,
	};

	if ((str = pw_properties_get(impl->props, "stream.rules")) == NULL)
		str = DEFAULT_CREATE_RULES;

	pw_conf_match_rules(str, strlen(str), NAME, &props->dict,
			rule_matched, &minfo);

	if (!minfo.matched) {
		pw_log_info("unmatched client found %s", str);
		if (conn)
			pw_websocket_connection_destroy(conn);
		pw_properties_free(props);
	}
	return minfo.matched;
}

static void on_websocket_connected(void *data, void *user,
		struct pw_websocket_connection *conn, const char *path)
{
	struct impl *impl = data;
	struct client *c = user;

	pw_log_info("connected to %s", path);
	if (c == NULL) {
		struct sockaddr_storage addr;
		char ip[128];
		uint16_t port = 0;
		bool ipv4;
		struct pw_properties *props;

		pw_websocket_connection_address(conn,
				(struct sockaddr*)&addr, sizeof(addr));

		props = pw_properties_copy(impl->stream_props);
		if (pw_net_get_ip(&addr, ip, sizeof(ip), &ipv4, &port) >= 0) {
			pw_properties_set(props, "sendspin.ip", ip);
			pw_properties_setf(props, "sendspin.port", "%u", port);
		}
		pw_properties_set(props, "sendspin.path", path);

		match_client(impl, "", props, conn);
	} else {
		client_connected(c, conn);
	}
}

static const struct pw_websocket_events websocket_events = {
	PW_VERSION_WEBSOCKET_EVENTS,
	.connected = on_websocket_connected,
};

static void on_zeroconf_added(void *data, void *user, const struct spa_dict *info)
{
	struct impl *impl = data;
	const char *name, *addr, *port, *path;
	struct client *c;
	struct pw_properties *props;

	name = spa_dict_lookup(info, "zeroconf.hostname");

	if ((c = client_find(impl, name)) != NULL)
		return;

	props = pw_properties_copy(impl->stream_props);
	pw_properties_update(props, info);

	addr = spa_dict_lookup(info, "zeroconf.address");
	port = spa_dict_lookup(info, "zeroconf.port");
	path = spa_dict_lookup(info, "path");

	pw_properties_set(props, "sendspin.ip", addr);
	pw_properties_set(props, "sendspin.port", port);
	pw_properties_set(props, "sendspin.path", path);

	match_client(impl, name, props, NULL);
}

static void on_zeroconf_removed(void *data, void *user, const struct spa_dict *info)
{
	struct impl *impl = data;
	const char *name;
	struct client *c;

	name = spa_dict_lookup(info, "zeroconf.hostname");

	if ((c = client_find(impl, name)) == NULL)
		return;

	client_free(c);
}

static const struct pw_zeroconf_events zeroconf_events = {
	PW_VERSION_ZEROCONF_EVENTS,
	.added = on_zeroconf_added,
	.removed = on_zeroconf_removed,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	struct client *c;
	spa_list_consume(c, &impl->clients, link)
		client_free(c);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->data_loop)
		pw_context_release_loop(impl->context, impl->data_loop);

	if (impl->zeroconf)
		pw_zeroconf_destroy(impl->zeroconf);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl);
}

static void module_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void on_core_error(void *d, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = d;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->stream_props, key) == NULL)
			pw_properties_set(impl->stream_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	const char *str, *hostname, *port, *path;
	struct pw_properties *props, *stream_props;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	if (args == NULL)
		args = "";

	props = impl->props = pw_properties_new_string(args);
	stream_props = impl->stream_props = pw_properties_new(NULL, NULL);
	if (props == NULL || stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}

	impl->module = module;
	impl->context = context;
	impl->main_loop = pw_context_get_main_loop(context);
	impl->data_loop = pw_context_acquire_loop(context, &props->dict);
	impl->timer_queue = pw_context_get_timer_queue(context);
	spa_list_init(&impl->clients);

	pw_properties_set(props, PW_KEY_NODE_LOOP_NAME, impl->data_loop->name);

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_NODE_LOOP_NAME);
	copy_props(impl, props, SPA_KEY_AUDIO_LAYOUT);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_CHANNELNAMES);
	copy_props(impl, props, PW_KEY_MEDIA_NAME);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);

	impl->always_process = pw_properties_get_bool(stream_props,
			PW_KEY_NODE_ALWAYS_PROCESS, true);

	if ((str = pw_properties_get(props, "sendspin.group-id")) == NULL) {
		uint64_t group_id;
		pw_random(&group_id, sizeof(group_id));
		pw_properties_setf(props, "sendspin.group-id", "%016"PRIx64, group_id);
	}
	if ((str = pw_properties_get(props, "sendspin.group-name")) == NULL)
		pw_properties_set(props, "sendspin.group-name", "PipeWire");
	if ((str = pw_properties_get(props, "sendspin.server-name")) == NULL)
		pw_properties_set(props, "sendspin.server-name", pw_get_host_name());
	if ((str = pw_properties_get(props, "sendspin.server-id")) == NULL)
		pw_properties_setf(props, "sendspin.server-id", "pipewire-%s", pw_get_host_name());

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto out;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	impl->websocket = pw_websocket_new(impl->main_loop, &props->dict);
	pw_websocket_add_listener(impl->websocket, &impl->websocket_listener,
			&websocket_events, impl);

	if ((str = pw_properties_get(props, "sendspin.delay")) == NULL)
		str = SPA_STRINGIFY(DEFAULT_SENDSPIN_DELAY);
	impl->delay = pw_properties_parse_float(str);

	if ((impl->zeroconf = pw_zeroconf_new(context, NULL)) != NULL) {
		pw_zeroconf_add_listener(impl->zeroconf, &impl->zeroconf_listener,
				&zeroconf_events, impl);
	}

	hostname = pw_properties_get(props, "sendspin.ip");
	if (hostname != NULL) {
		struct spa_json iter;
		char v[256];

		port = pw_properties_get(props, "sendspin.port");
		if (port == NULL)
			port = SPA_STRINGIFY(DEFAULT_CLIENT_PORT);
		if ((path = pw_properties_get(props, "sendspin.path")) == NULL)
			path = DEFAULT_SENDSPIN_PATH;

		if (spa_json_begin_array_relax(&iter, hostname, strlen(hostname)) <= 0) {
			res = -EINVAL;
			pw_log_error("can't parse sendspin.ip %s", hostname);
			goto out;
		}
		while (spa_json_get_string(&iter, v, sizeof(v)) > 0) {
			struct client *c;
			struct pw_properties *p = pw_properties_copy(impl->stream_props);

			pw_properties_set(p, "sendspin.ip", v);
			pw_properties_set(p, "sendspin.port", port);
			pw_properties_set(p, "sendspin.path", path);

			if ((c = client_new(impl, "", p)) != NULL)
				client_connect(c);
		}
	} else {
		if ((hostname = pw_properties_get(props, "source.ip")) == NULL)
			hostname = DEFAULT_SOURCE_IP;
		if ((port = pw_properties_get(props, "source.port")) == NULL)
			port = SPA_STRINGIFY(DEFAULT_SOURCE_PORT);
		if ((path = pw_properties_get(props, "source.path")) == NULL)
			path = DEFAULT_SOURCE_PATH;

		pw_websocket_listen(impl->websocket, NULL, hostname, port, path);

		if (impl->zeroconf) {
			str = pw_properties_get(props, "sendspin.group-name");
			pw_zeroconf_set_announce(impl->zeroconf, NULL,
				&SPA_DICT_ITEMS(
					SPA_DICT_ITEM("zeroconf.service", PW_SENDSPIN_SERVER_SERVICE),
					SPA_DICT_ITEM("zeroconf.session", str),
					SPA_DICT_ITEM("zeroconf.port", port),
					SPA_DICT_ITEM("path", path)));
		}
	}
	if (impl->zeroconf) {
		pw_zeroconf_set_browse(impl->zeroconf, NULL,
			&SPA_DICT_ITEMS(
				SPA_DICT_ITEM("zeroconf.service", PW_SENDSPIN_CLIENT_SERVICE)));
	}
	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-sendspin-send");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
