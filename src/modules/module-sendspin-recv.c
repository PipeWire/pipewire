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

#include "zeroconf-utils/zeroconf.h"
#include "module-sendspin/sendspin.h"
#include "module-sendspin/websocket.h"
#include "module-sendspin/regress.h"
#include "network-utils.h"

/** \page page_module_sendspin_recv sendspin receiver
 *
 * The `sendspin-recv` module creates a PipeWire source that receives audio
 * packets using the sendspin protocol.
 *
 * The receive will listen on a specific port (8928) and create a stream for the
 * data on the port.
 *
 * ## Module Name
 *
 * `libpipewire-module-sendspin-recv`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `local.ifname = <str>`: interface name to use
 * - `source.ip = <str>`: the source ip address to listen on, default 127.0.0.1
 * - `source.port = <int>`: the source port to listen on, default 8928
 * - `source.path = <str>`: the path to listen on, default "/sendspin"
 * - `sendspin.ip`: the IP address of the sendspin server
 * - `sendspin.port`: the port of the sendspin server, default 8927
 * - `sendspin.path`: the path on the sendspin server, default "/sendspin"
 * - `sendspin.client-id`: the client id, default "pipewire-$(hostname)"
 * - `sendspin.client-name`: the client name, default "$(hostname)"
 * - `sendspin.autoconnect`: Use zeroconf to connect to an available server, default false.
 * - `sendspin.announce`: Use zeroconf to announce the client, default true unless
 *                      sendspin.autoconnect or sendspin.ip is given.
 * - `sendspin.single-server`: Allow only a single server to connect, default true
 * - `node.always-process = <bool>`: true to receive even when not running
 * - `stream.props = {}`: properties to be passed to all the stream
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
 * # ~/.config/pipewire/pipewire.conf.d/my-sendspin-recv.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-sendspin-recv
 *     args = {
 *         #local.ifname = eth0
 *         #source.ip = 127.0.0.1
 *         #source.port = 8928
 *         #source.path = "/sendspin"
 *         #sendspin.ip = 127.0.0.1
 *         #sendspin.port = 8927
 *         #sendspin.path = "/sendspin"
 *         #sendspin.client-id = "pipewire-test"
 *         #sendspin.client-name = "PipeWire Test"
 *         #sendspin.autoconnect = false
 *         #sendspin.announce = true
 *         #sendspin.single-server = true
 *         #node.always-process = false
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *            #media.class = "Audio/Source"
 *            #node.name = "sendspin-receiver"
 *         }
 *     }
 * }
 * ]
 *\endcode
 *
 * \since 1.6.0
 */

#define NAME "sendspin-recv"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_SOURCE_IP	"127.0.0.1"
#define DEFAULT_SOURCE_PORT	PW_SENDSPIN_DEFAULT_CLIENT_PORT
#define DEFAULT_SOURCE_PATH	PW_SENDSPIN_DEFAULT_PATH

#define DEFAULT_SERVER_PORT	PW_SENDSPIN_DEFAULT_SERVER_PORT
#define DEFAULT_SENDSPIN_PATH	PW_SENDSPIN_DEFAULT_PATH

#define DEFAULT_CREATE_RULES	\
        "[ { matches = [ { sendspin.ip = \"~.*\" } ] actions = { create-stream = { } } } ] "

#define DEFAULT_POSITION	"[ FL FR ]"

#define USAGE   "( local.ifname=<local interface name to use> ) "						\
		"( source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> ) "				\
 		"( source.port=<int, source port, default:"SPA_STRINGIFY(DEFAULT_SOURCE_PORT)"> "		\
		"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "					\
		"( stream.props= { key=value ... } ) "

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@proton.me>" },
	{ PW_KEY_MODULE_DESCRIPTION, "sendspin Receiver" },
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

	struct pw_timer timer;
	int timeout_count;

	uint32_t stride;
	struct spa_ringbuffer ring;
	void *buffer;
	uint32_t buffer_size;

#define ROLE_PLAYER		(1<<0)
#define ROLE_METADATA		(1<<1)
	uint32_t active_roles;
#define REASON_DISCOVERY	(0)
#define REASON_PLAYBACK		(1)
	uint32_t connection_reason;

	struct spa_regress regress_index;
	struct spa_regress regress_time;

	bool resync;
	struct spa_dll dll;
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

	bool always_process;
	bool single_server;

	struct pw_properties *stream_props;

	struct pw_websocket *websocket;
	struct spa_hook websocket_listener;

	struct spa_list clients;
};

static void on_stream_destroy(void *d)
{
	struct client *client = d;
	spa_hook_remove(&client->stream_listener);
	client->stream = NULL;
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct client *client = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(client->impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
	case PW_STREAM_STATE_STREAMING:
		break;
	default:
		break;
	}
}

static void on_capture_stream_process(void *d)
{
	struct client *client = d;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *p;
	uint32_t index = 0, n_frames, n_bytes;
	int32_t avail, stride;
	struct pw_time ts;
	double err, corr, target, current_time;

	if ((b = pw_stream_dequeue_buffer(client->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL)
		return;

	stride = client->stride;
	n_frames = buf->datas[0].maxsize / stride;
	if (b->requested)
		n_frames = SPA_MIN(b->requested, n_frames);
	n_bytes = n_frames * stride;

	avail = spa_ringbuffer_get_read_index(&client->ring, &index);

	if (client->timeout_count > 4 && client->timeout_count > 4) {
		pw_stream_get_time_n(client->stream, &ts, sizeof(ts));

		/* index to server time */
		target = spa_regress_calc_y(&client->regress_index, index);
		/* server time to client time */
		target = spa_regress_calc_y(&client->regress_time, target);

		current_time = ts.now / 1000.0;
		current_time -= (ts.buffered * 1000000.0 / client->info.info.raw.rate) +
			((ts.delay) * 1000000.0 * ts.rate.num / ts.rate.denom);
		err = target - (double)current_time;

		if (client->resync) {
			if (target < current_time) {
				target = spa_regress_calc_x(&client->regress_time, current_time);
				index = (uint32_t)spa_regress_calc_x(&client->regress_index, target);
				index = SPA_ROUND_DOWN(index, stride);

				pw_log_info("resync %u %f %f %f", index, target,
						current_time, target - current_time);

				spa_ringbuffer_read_update(&client->ring, index);
				avail = spa_ringbuffer_get_read_index(&client->ring, &index);

				err = 0.0;
				client->resync = false;
			} else {
				avail = 0;
			}
		}
	} else {
		avail = 0;
	}
	if (avail < (int32_t)n_bytes) {
		avail = 0;
		client->resync = true;
	}
	else if (avail > (int32_t)client->buffer_size) {
		index += avail - client->buffer_size;
		avail = client->buffer_size;
		client->resync = true;
	}
	if (avail > 0) {
		n_bytes = SPA_MIN(n_bytes, (uint32_t)avail);

		corr = spa_dll_update(&client->dll, SPA_CLAMPD(err, -1000, 1000));

		pw_log_trace("%u %f %f %f %f", index, current_time, target, err, corr);

		pw_stream_set_rate(client->stream, 1.0 / corr);

		spa_ringbuffer_read_data(&client->ring,
				client->buffer, client->buffer_size,
				index % client->buffer_size,
				p, n_bytes);
		spa_ringbuffer_read_update(&client->ring, index + n_bytes);
	} else {
		memset(p, 0, n_bytes);
	}

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n_bytes;

	pw_stream_queue_buffer(client->stream, b);
}

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.state_changed = on_stream_state_changed,
	.process = on_capture_stream_process
};

static int create_stream(struct client *client)
{
	struct impl *impl = client->impl;
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const char *server_id, *ip, *port, *server_name;
	struct pw_properties *props = pw_properties_copy(client->props);

	ip = pw_properties_get(props, "sendspin.ip");
	port = pw_properties_get(props, "sendspin.port");
	server_id = pw_properties_get(props, "sendspin.server-id");
	server_name = pw_properties_get(props, "sendspin.server-name");

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "sendspin.%s.%s.%s", server_id, ip, port);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "Sendspin from %s", server_name);
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "Sendspin from %s", server_name);

	client->stream = pw_stream_new(impl->core, "sendspin receiver", props);
	if (client->stream == NULL)
		return -errno;

	spa_ringbuffer_init(&client->ring);
	client->buffer_size = 1024 * 1024;
	client->buffer = calloc(1, client->buffer_size * client->stride);

	pw_stream_add_listener(client->stream,
			&client->stream_listener,
			&capture_stream_events, client);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_build(&b,
			SPA_PARAM_EnumFormat, &client->info);

	if ((res = pw_stream_connect(client->stream,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static void add_format(struct spa_json_builder *b, const char *codec, int channels, int rate, int depth)
{
	spa_json_builder_array_push(b,  "{");
	spa_json_builder_object_string(b, "codec", codec);
	spa_json_builder_object_int(b,    "channels", channels);
	spa_json_builder_object_int(b,    "sample_rate", rate);
	spa_json_builder_object_int(b,    "bit_depth", depth);
	spa_json_builder_pop(b,         "}");
}
static void add_playerv1_support(struct client *client, struct spa_json_builder *b)
{
	spa_json_builder_object_push(b, "player@v1_support", "{");
	spa_json_builder_object_push(b,   "supported_formats", "[");
	add_format(b,                       "pcm", 2, 48000, 16);
	add_format(b,                       "pcm", 1, 48000, 16);
	spa_json_builder_pop(b,           "]");
	spa_json_builder_object_int(b,    "buffer_capacity", 32000000);
	spa_json_builder_object_push(b,   "supported_commands", "[");
	spa_json_builder_array_string(b,    "volume");
	spa_json_builder_array_string(b,    "mute");
	spa_json_builder_pop(b,           "]");
	spa_json_builder_pop(b,         "}");
}
static int send_client_hello(struct client *client)
{
	struct impl *impl = client->impl;
	struct spa_json_builder b;
	int res;
	char *mem;
	size_t size;

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "client/hello");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_string(&b,   "client_id", pw_properties_get(impl->props, "sendspin.client-id"));
	spa_json_builder_object_string(&b,   "name", pw_properties_get(impl->props, "sendspin.client-name"));
	spa_json_builder_object_int(&b,      "version", 1);
	spa_json_builder_object_push(&b,     "supported_roles", "[");
	spa_json_builder_array_string(&b,      "player@v1");
	spa_json_builder_array_string(&b,      "metadata@v1");
	spa_json_builder_pop(&b,             "]");
	spa_json_builder_object_push(&b,     "device_info", "{");
	spa_json_builder_object_string(&b,     "product_name", "Linux"); /* Use os-release */
	spa_json_builder_object_stringf(&b,    "software_version", "PipeWire %s", pw_get_library_version());
	spa_json_builder_pop(&b,             "}");
	add_playerv1_support(client, &b);
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(client->conn, mem, size);
	free(mem);

	return res;
}

static int send_client_state(struct client *client)
{
	struct spa_json_builder b;
	int res;
	char *mem;
	size_t size;

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "client/state");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_push(&b,     "player", "{");
	spa_json_builder_object_string(&b,     "state", "synchronized");
	spa_json_builder_object_int(&b,        "volume", 100);
	spa_json_builder_object_bool(&b,       "muted", false);
	spa_json_builder_pop(&b,             "}");
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(client->conn, mem, size);
	free(mem);
	return res;
}

static uint64_t get_time_us(struct client *client)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return SPA_TIMESPEC_TO_USEC(&now);
}

static int send_client_time(struct client *client)
{
	struct spa_json_builder b;
	int res;
	uint64_t now;
	char *mem;
	size_t size;

	now = get_time_us(client);

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "client/time");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_uint(&b,     "client_transmitted", now);
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(client->conn, mem, size);
	free(mem);
	return res;
}

static void do_client_timer(void *data)
{
	struct client *client = data;
	send_client_time(client);
}

#if 0
static int send_client_command(struct client *client)
{
	return 0;
}
#endif
static int send_client_goodbye(struct client *client, const char *reason)
{
	struct spa_json_builder b;
	int res;
	char *mem;
	size_t size;

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "client/goodbye");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_string(&b,   "reason", reason);
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(client->conn, mem, size);
	pw_websocket_connection_disconnect(client->conn, true);
	free(mem);
	return res;
}

#if 0
static int send_stream_request_format(struct client *client)
{
	return 0;
}
#endif

static int handle_server_hello(struct client *client, struct spa_json *payload)
{
	struct impl *impl = client->impl;
	struct spa_json it[1];
	char key[256], *t;
        const char *v;
	int l, version = 0;
	struct client *c, *ct;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "server_id")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);
			pw_properties_set(client->props, "sendspin.server-id", t);
		}
		else if (spa_streq(key, "name")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);
			pw_properties_set(client->props, "sendspin.server-name", t);
		}
		else if (spa_streq(key, "version")) {
			if (spa_json_parse_int(v, l, &version) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "active_roles")) {
			if (!spa_json_is_array(v, l))
				return -EPROTO;

			spa_json_enter(payload, &it[0]);
			while ((l = spa_json_next(&it[0], &v)) > 0) {
				t = alloca(l+1);
				spa_json_parse_stringn(v, l, t, l+1);

				if (spa_streq(t, "player@v1"))
					client->active_roles |= ROLE_PLAYER;
				else if (spa_streq(t, "metadata@v1"))
					client->active_roles |= ROLE_METADATA;
			}
		}
		else if (spa_streq(key, "connection_reason")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);

			if (spa_streq(t, "discovery"))
				client->connection_reason = REASON_DISCOVERY;
			else if (spa_streq(t, "playback"))
				client->connection_reason = REASON_PLAYBACK;

			pw_properties_set(client->props, "sendspin.connection-reason", t);
		}
	}
	if (version != 1)
		return -ENOTSUP;

	if (impl->single_server) {
		if (client->connection_reason == REASON_PLAYBACK) {
			/* keep this server, destroy others */
			spa_list_for_each_safe(c, ct, &impl->clients, link) {
				if (c == client)
					continue;
				send_client_goodbye(c, "another_server");
			}
		} else {
			/* keep other servers, destroy this one */
			spa_list_for_each_safe(c, ct, &impl->clients, link) {
				if (c == client)
					continue;
				return send_client_goodbye(client, "another_server");
			}
		}
	}
	return send_client_state(client);
}

static int handle_server_state(struct client *client, struct spa_json *payload)
{
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

static int handle_server_time(struct client *client, struct spa_json *payload)
{
	struct impl *impl = client->impl;
	char key[256];
        const char *v;
	int l;
	uint64_t t1 = 0, t2 = 0, t3 = 0, t4 = 0, timeout;

	t4 = get_time_us(client);

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "client_transmitted")) {
			if (parse_uint64(v, l, &t1) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "server_received")) {
			if (parse_uint64(v, l, &t2) <= 0)
                                return -EINVAL;
		}
		else if (spa_streq(key, "server_transmitted")) {
			if (parse_uint64(v, l, &t3) <= 0)
                                return -EINVAL;
		}
	}

	spa_regress_update(&client->regress_time, (t2+t3)/2, (t1+t4)/2);

	if (client->timeout_count < 4)
		timeout = 200 * SPA_MSEC_PER_SEC;
	else if (client->timeout_count < 10)
		timeout = SPA_NSEC_PER_SEC;
	else if (client->timeout_count < 20)
		timeout = 2 * SPA_NSEC_PER_SEC;
	else
		timeout = 5 * SPA_NSEC_PER_SEC;

	client->timeout_count++;
	pw_timer_queue_add(impl->timer_queue, &client->timer,
			&client->timer.timeout, timeout,
			do_client_timer, client);
	return 0;
}

static int handle_server_command(struct client *client, struct spa_json *payload)
{
	return 0;
}

/* {"codec":"pcm","sample_rate":44100,"channels":2,"bit_depth":16} */
static int parse_player(struct client *client, struct spa_json *player)
{
	char key[256], codec[64] = "";
        const char *v;
	int l, sample_rate = 0, channels = 0, bit_depth = 0;

	spa_zero(client->info);
	client->info.media_type = SPA_MEDIA_TYPE_audio;
	while ((l = spa_json_object_next(player, key, sizeof(key), &v)) > 0) {
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
		client->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		client->info.info.raw.rate = sample_rate;
		client->info.info.raw.channels = channels;
		switch (bit_depth) {
		case 16:
			client->info.info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
			client->stride = 2 * channels;
			break;
		case 24:
			client->info.info.raw.format = SPA_AUDIO_FORMAT_S24_LE;
			client->stride = 3 * channels;
			break;
		default:
			return -EINVAL;
		}
	}
	else if (spa_streq(codec, "opus")) {
		client->info.media_subtype = SPA_MEDIA_SUBTYPE_opus;
		client->info.info.opus.rate = sample_rate;
		client->info.info.opus.channels = channels;
	}
	else if (spa_streq(codec, "flac")) {
		client->info.media_subtype = SPA_MEDIA_SUBTYPE_flac;
		client->info.info.flac.rate = sample_rate;
		client->info.info.flac.channels = channels;
	}
	else
		return -EINVAL;

	spa_dll_set_bw(&client->dll, SPA_DLL_BW_MIN, 1000, sample_rate);

	return 0;
}

/* {"player":{}} */
static int handle_stream_start(struct client *client, struct spa_json *payload)
{
	struct impl *impl = client->impl;
	struct spa_json it[1];
	char key[256];
        const char *v;
	int l;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "player")) {
			if (!spa_json_is_object(v, l))
				return -EPROTO;
			spa_json_enter(payload, &it[0]);
			parse_player(client, &it[0]);
		}
	}

	if (client->stream == NULL) {
		create_stream(client);

		pw_timer_queue_cancel(&client->timer);
		pw_timer_queue_add(impl->timer_queue, &client->timer,
			NULL, 0, do_client_timer, client);
	} else {
	}

	return 0;
}

static void stream_clear(struct client *client)
{
	spa_ringbuffer_init(&client->ring);
	memset(client->buffer, 0, client->buffer_size);
}

static int handle_stream_clear(struct client *client, struct spa_json *payload)
{
	stream_clear(client);
	return 0;
}
static int handle_stream_end(struct client *client, struct spa_json *payload)
{
	if (client->stream != NULL) {
		pw_stream_destroy(client->stream);
		client->stream = NULL;
		stream_clear(client);
	}
	return 0;
}

static int handle_group_update(struct client *client, struct spa_json *payload)
{
	return 0;
}

/* { "type":... "payload":{...} } */
static int do_parse_text(struct client *client, const char *content, int size)
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
	if (spa_streq(type, "server/hello"))
		res = handle_server_hello(client, payload);
	else if (spa_streq(type, "server/state"))
		res = handle_server_state(client, payload);
	else if (spa_streq(type, "server/time"))
		res = handle_server_time(client, payload);
	else if (spa_streq(type, "server/command"))
		res = handle_server_command(client, payload);
	else if (spa_streq(type, "stream/start"))
		res = handle_stream_start(client, payload);
	else if (spa_streq(type, "stream/end"))
		res = handle_stream_end(client, payload);
	else if (spa_streq(type, "stream/clear"))
		res = handle_stream_clear(client, payload);
	else if (spa_streq(type, "group/update"))
		res = handle_group_update(client, payload);
	else
		res = 0;

	return res;
}

static int do_handle_binary(struct client *client, const uint8_t *payload, int size)
{
	struct impl *impl = client->impl;
	int32_t filled;
	uint32_t index, length = size - 9;
	uint64_t timestamp;

	if (payload[0] != 4 || client->stream == NULL)
		return 0;

	timestamp  = ((uint64_t)payload[1]) << 56;
	timestamp |= ((uint64_t)payload[2]) << 48;
	timestamp |= ((uint64_t)payload[3]) << 40;
	timestamp |= ((uint64_t)payload[4]) << 32;
	timestamp |= ((uint64_t)payload[5]) << 24;
	timestamp |= ((uint64_t)payload[6]) << 16;
	timestamp |= ((uint64_t)payload[7]) <<  8;
	timestamp |= ((uint64_t)payload[8]);

	filled = spa_ringbuffer_get_write_index(&client->ring, &index);
	if (filled < 0) {
		pw_log_warn("%p: underrun write:%u filled:%d",
				client, index, filled);
	} else if (filled + length > client->buffer_size) {
		pw_log_debug("%p: overrun write:%u filled:%d",
				client, index, filled);
	}

	spa_ringbuffer_write_data(&client->ring,
			client->buffer, client->buffer_size,
			index % client->buffer_size,
			&payload[9], length);

	spa_ringbuffer_write_update(&client->ring, index + length);

	pw_loop_lock(impl->data_loop);
	spa_regress_update(&client->regress_index, index, timestamp);
	pw_loop_unlock(impl->data_loop);

	return 0;
}

static void on_connection_message(void *data, int opcode, void *payload, size_t size)
{
	struct client *client = data;
	if (opcode == PW_WEBSOCKET_OPCODE_TEXT) {
		do_parse_text(client, payload, size);
	} else if (opcode == PW_WEBSOCKET_OPCODE_BINARY) {
		do_handle_binary(client, payload, size);
	} else {
		pw_log_warn("%02x unknown %08x", opcode, (int)size);
	}
}

static void client_free(struct client *client)
{
	struct impl *impl = client->impl;

	spa_list_remove(&client->link);

	handle_stream_end(client, NULL);
	if (client->conn) {
		spa_hook_remove(&client->conn_listener);
		pw_websocket_connection_destroy(client->conn);
	} else {
		pw_websocket_cancel(impl->websocket, client);
	}
	pw_timer_queue_cancel(&client->timer);

	pw_properties_free(client->props);
	free(client->buffer);
	free(client->name);
	free(client);
}

static void on_connection_destroy(void *data)
{
	struct client *client = data;
	client->conn = NULL;
	pw_log_info("connection %p destroy", client);
}
static void on_connection_error(void *data, int res, const char *reason)
{
	struct client *client = data;
	pw_log_error("connection %p error %d %s", client, res, reason);
}

static void on_connection_disconnected(void *data)
{
	struct client *client = data;
	client_free(client);
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
	struct client *client;

	client = calloc(1, sizeof(*client));
	if (client == NULL)
		goto error;

	client->impl = impl;
	spa_list_append(&impl->clients, &client->link);

	client->name = name ? strdup(name) : NULL;
	client->props = props;
	spa_regress_init(&client->regress_index, 5);
	spa_regress_init(&client->regress_time, 5);

	spa_dll_init(&client->dll);
	client->resync = true;

	return client;
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

		if ((c = client_new(impl, "", props)) == NULL) {
			pw_log_error("can't create new client: %m");
			return;
		}
	}
	client_connected(c, conn);
	send_client_hello(c);
}

static const struct pw_websocket_events websocket_events = {
	PW_VERSION_WEBSOCKET_EVENTS,
	.connected = on_websocket_connected,
};

static void on_zeroconf_added(void *data, const void *user, const struct spa_dict *info)
{
	struct impl *impl = data;
	const char *name, *addr, *port, *path;
	struct client *c;
	struct pw_properties *props;

	name = spa_dict_lookup(info, "zeroconf.hostname");

	if (impl->single_server && !spa_list_is_empty(&impl->clients))
		return;

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

static void on_zeroconf_removed(void *data, const void *user, const struct spa_dict *info)
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
	bool autoconnect, announce;

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

	autoconnect = pw_properties_get_bool(props, "sendspin.autoconnect", false);
	announce = pw_properties_get_bool(props, "sendspin.announce", true);
	impl->single_server = pw_properties_get_bool(props,
			"sendspin.single-server", true);

	if ((str = pw_properties_get(props, "sendspin.client-name")) == NULL)
		pw_properties_set(props, "sendspin.client-name", pw_get_host_name());
	if ((str = pw_properties_get(props, "sendspin.client-id")) == NULL)
		pw_properties_setf(props, "sendspin.client-id", "pipewire-%s", pw_get_host_name());

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

	if ((impl->zeroconf = pw_zeroconf_new(context, NULL)) != NULL) {
		pw_zeroconf_add_listener(impl->zeroconf, &impl->zeroconf_listener,
				&zeroconf_events, impl);
	}

	hostname = pw_properties_get(props, "sendspin.ip");
	/* a client should either connect itself or advertize itself and listen
	 * for connections, not both */
	if (!autoconnect && hostname == NULL){
		/* listen for server connection */
		if ((hostname = pw_properties_get(props, "source.ip")) == NULL)
			hostname = DEFAULT_SOURCE_IP;
		if ((port = pw_properties_get(props, "source.port")) == NULL)
			port = SPA_STRINGIFY(DEFAULT_SOURCE_PORT);
		if ((path = pw_properties_get(props, "source.path")) == NULL)
			path = DEFAULT_SOURCE_PATH;

		pw_websocket_listen(impl->websocket, NULL, hostname, port, path);

		if (impl->zeroconf && announce) {
			/* optionally announce ourselves */
			str = pw_properties_get(props, "sendspin.client-id");
			pw_zeroconf_set_announce(impl->zeroconf, NULL,
				&SPA_DICT_ITEMS(
					SPA_DICT_ITEM("zeroconf.service", PW_SENDSPIN_CLIENT_SERVICE),
					SPA_DICT_ITEM("zeroconf.session", str),
					SPA_DICT_ITEM("zeroconf.port", port),
					SPA_DICT_ITEM("path", path)));
		}
	}
	else {
		if (hostname != NULL) {
			struct client *c;
			struct pw_properties *p;

			/* connect to hardcoded server */
			port = pw_properties_get(props, "sendspin.port");
			if (port == NULL)
				port = SPA_STRINGIFY(DEFAULT_SERVER_PORT);
			if ((path = pw_properties_get(props, "sendspin.path")) == NULL)
				path = DEFAULT_SENDSPIN_PATH;

			p = pw_properties_copy(impl->stream_props);
			pw_properties_set(p, "sendspin.ip", hostname);
			pw_properties_set(p, "sendspin.port", port);
			pw_properties_set(p, "sendspin.path", path);

			if ((c = client_new(impl, "", p)) != NULL)
				client_connect(c);
		}
		/* connect to zeroconf server if we can */
		if (impl->zeroconf) {
			pw_zeroconf_set_browse(impl->zeroconf, NULL,
				&SPA_DICT_ITEMS(
					SPA_DICT_ITEM("zeroconf.service", PW_SENDSPIN_SERVER_SERVICE)));
		}
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-sendspin-recv");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
