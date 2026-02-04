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
#include "module-sendspin/zeroconf.h"
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

struct stream {
	struct impl *impl;
	struct spa_list link;

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

	struct pw_properties *stream_props;

	struct pw_websocket *websocket;
	struct spa_hook websocket_listener;

	struct spa_list streams;
};

static void on_stream_destroy(void *d)
{
	struct stream *stream = d;
	spa_hook_remove(&stream->stream_listener);
	stream->stream = NULL;
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct stream *stream = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(stream->impl->module);
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
	struct stream *stream = d;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *p;
	uint32_t index = 0, n_frames, n_bytes;
	int32_t avail, stride;
	struct pw_time ts;
	double err, corr, target, current_time;

	if ((b = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL)
		return;

	stride = stream->stride;
	n_frames = buf->datas[0].maxsize / stride;
	if (b->requested)
		n_frames = SPA_MIN(b->requested, n_frames);
	n_bytes = n_frames * stride;

	avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

	if (stream->timeout_count > 4 && stream->timeout_count > 4) {
		pw_stream_get_time_n(stream->stream, &ts, sizeof(ts));

		/* index to server time */
		target = spa_regress_calc_y(&stream->regress_index, index);
		/* server time to client time */
		target = spa_regress_calc_y(&stream->regress_time, target);

		current_time = ts.now / 1000.0;
		current_time -= (ts.buffered * 1000000.0 / stream->info.info.raw.rate) +
			((ts.delay) * 1000000.0 * ts.rate.num / ts.rate.denom);
		err = target - (double)current_time;

		if (stream->resync) {
			if (target < current_time) {
				target = spa_regress_calc_x(&stream->regress_time, current_time);
				index = (uint32_t)spa_regress_calc_x(&stream->regress_index, target);
				index = SPA_ROUND_DOWN(index, stride);

				pw_log_info("resync %u %f %f %f", index, target,
						current_time, target - current_time);

				spa_ringbuffer_read_update(&stream->ring, index);
				avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

				err = 0.0;
				stream->resync = false;
			} else {
				avail = 0;
			}
		}
	} else {
		avail = 0;
	}
	if (avail < (int32_t)n_bytes) {
		avail = 0;
		stream->resync = true;
	}
	else if (avail > (int32_t)stream->buffer_size) {
		index += avail - stream->buffer_size;
		avail = stream->buffer_size;
		stream->resync = true;
	}
	if (avail > 0) {
		n_bytes = SPA_MIN(n_bytes, (uint32_t)avail);

		corr = spa_dll_update(&stream->dll, SPA_CLAMPD(err, -1000, 1000));

		pw_log_trace("%u %f %f %f %f", index, current_time, target, err, corr);

		pw_stream_set_rate(stream->stream, 1.0 / corr);

		spa_ringbuffer_read_data(&stream->ring,
				stream->buffer, stream->buffer_size,
				index % stream->buffer_size,
				p, n_bytes);
		spa_ringbuffer_read_update(&stream->ring, index + n_bytes);
	} else {
		memset(p, 0, n_bytes);
	}

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n_bytes;

	pw_stream_queue_buffer(stream->stream, b);
}

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.state_changed = on_stream_state_changed,
	.process = on_capture_stream_process
};

static int create_stream(struct stream *stream)
{
	struct impl *impl = stream->impl;
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const char *server_id, *ip, *port, *server_name;
	struct pw_properties *props = pw_properties_copy(impl->stream_props);

	ip = pw_properties_get(impl->props, "sendspin.ip");
	port = pw_properties_get(impl->props, "sendspin.port");
	server_id = pw_properties_get(props, "sendspin.server-id");
	server_name = pw_properties_get(props, "sendspin.server-name");

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "sendspin.%s.%s.%s", server_id, ip, port);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "Sendspin from %s", server_name);
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "Sendspin from %s", server_name);

	stream->stream = pw_stream_new(impl->core, "sendspin receiver", props);
	if (stream->stream == NULL)
		return -errno;

	spa_ringbuffer_init(&stream->ring);
	stream->buffer_size = 1024 * 1024;
	stream->buffer = calloc(1, stream->buffer_size * stream->stride);

	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&capture_stream_events, stream);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_build(&b,
			SPA_PARAM_EnumFormat, &stream->info);

	if ((res = pw_stream_connect(stream->stream,
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
static void add_playerv1_support(struct stream *stream, struct spa_json_builder *b)
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
static int send_client_hello(struct stream *stream)
{
	struct impl *impl = stream->impl;
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
	add_playerv1_support(stream, &b);
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(stream->conn, mem, size);
	free(mem);

	return res;
}

static int send_client_state(struct stream *stream)
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

	res = pw_websocket_connection_send_text(stream->conn, mem, size);
	free(mem);
	return res;
}

static uint64_t get_time_us(struct stream *stream)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return SPA_TIMESPEC_TO_USEC(&now);
}

static int send_client_time(struct stream *stream)
{
	struct spa_json_builder b;
	int res;
	uint64_t now;
	char *mem;
	size_t size;

	now = get_time_us(stream);

	spa_json_builder_memstream(&b, &mem, &size, 0);
	spa_json_builder_array_push(&b,  "{");
	spa_json_builder_object_string(&b, "type", "client/time");
	spa_json_builder_object_push(&b,   "payload", "{");
	spa_json_builder_object_uint(&b,     "client_transmitted", now);
	spa_json_builder_pop(&b,           "}");
	spa_json_builder_pop(&b,         "}");
	spa_json_builder_close(&b);

	res = pw_websocket_connection_send_text(stream->conn, mem, size);
	free(mem);
	return res;
}

static void do_stream_timer(void *data)
{
	struct stream *stream = data;
	send_client_time(stream);
}

#if 0
static int send_client_command(struct stream *stream)
{
	return 0;
}
#endif
static int send_client_goodbye(struct stream *stream, const char *reason)
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

	res = pw_websocket_connection_send_text(stream->conn, mem, size);
	pw_websocket_connection_disconnect(stream->conn, true);
	free(mem);
	return res;
}

#if 0
static int send_stream_request_format(struct stream *stream)
{
	return 0;
}
#endif

static int handle_server_hello(struct stream *stream, struct spa_json *payload)
{
	struct impl *impl = stream->impl;
	struct spa_json it[1];
	char key[256], *t;
        const char *v;
	int l, version = 0;
	struct stream *s, *st;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "server_id")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);
			pw_properties_set(impl->stream_props, "sendspin.server-id", t);
		}
		else if (spa_streq(key, "name")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);
			pw_properties_set(impl->stream_props, "sendspin.server-name", t);
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
					stream->active_roles |= ROLE_PLAYER;
				else if (spa_streq(t, "metadata@v1"))
					stream->active_roles |= ROLE_METADATA;
			}
		}
		else if (spa_streq(key, "connection_reason")) {
			t = alloca(l+1);
			spa_json_parse_stringn(v, l, t, l+1);

			if (spa_streq(t, "discovery"))
				stream->connection_reason = REASON_DISCOVERY;
			else if (spa_streq(t, "playback"))
				stream->connection_reason = REASON_PLAYBACK;

			pw_properties_set(impl->stream_props, "sendspin.connection-reason", t);
		}
	}
	if (version != 1)
		return -ENOTSUP;

	if (stream->connection_reason == REASON_PLAYBACK) {
		/* keep this server, destroy others */
		spa_list_for_each_safe(s, st, &impl->streams, link) {
			if (s == stream)
				continue;
			send_client_goodbye(s, "another_server");
		}
	} else {
		/* keep other servers, destroy this one */
		spa_list_for_each_safe(s, st, &impl->streams, link) {
			if (s == stream)
				continue;
			return send_client_goodbye(stream, "another_server");
		}
	}
	return send_client_state(stream);
}

static int handle_server_state(struct stream *stream, struct spa_json *payload)
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

static int handle_server_time(struct stream *stream, struct spa_json *payload)
{
	struct impl *impl = stream->impl;
	char key[256];
        const char *v;
	int l;
	uint64_t t1 = 0, t2 = 0, t3 = 0, t4 = 0, timeout;

	t4 = get_time_us(stream);

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

	spa_regress_update(&stream->regress_time, (t2+t3)/2, (t1+t4)/2);

	if (stream->timeout_count < 4)
		timeout = 200 * SPA_MSEC_PER_SEC;
	else if (stream->timeout_count < 10)
		timeout = SPA_NSEC_PER_SEC;
	else if (stream->timeout_count < 20)
		timeout = 2 * SPA_NSEC_PER_SEC;
	else
		timeout = 5 * SPA_NSEC_PER_SEC;

	stream->timeout_count++;
	pw_timer_queue_add(impl->timer_queue, &stream->timer,
			&stream->timer.timeout, timeout,
			do_stream_timer, stream);
	return 0;
}

static int handle_server_command(struct stream *stream, struct spa_json *payload)
{
	return 0;
}

/* {"codec":"pcm","sample_rate":44100,"channels":2,"bit_depth":16} */
static int parse_player(struct stream *stream, struct spa_json *player)
{
	char key[256], codec[64] = "";
        const char *v;
	int l, sample_rate = 0, channels = 0, bit_depth = 0;

	spa_zero(stream->info);
	stream->info.media_type = SPA_MEDIA_TYPE_audio;
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
		stream->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		stream->info.info.raw.rate = sample_rate;
		stream->info.info.raw.channels = channels;
		switch (bit_depth) {
		case 16:
			stream->info.info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
			stream->stride = 2 * channels;
			break;
		case 24:
			stream->info.info.raw.format = SPA_AUDIO_FORMAT_S24_LE;
			stream->stride = 3 * channels;
			break;
		default:
			return -EINVAL;
		}
	}
	else if (spa_streq(codec, "opus")) {
		stream->info.media_subtype = SPA_MEDIA_SUBTYPE_opus;
		stream->info.info.opus.rate = sample_rate;
		stream->info.info.opus.channels = channels;
	}
	else if (spa_streq(codec, "flac")) {
		stream->info.media_subtype = SPA_MEDIA_SUBTYPE_flac;
		stream->info.info.flac.rate = sample_rate;
		stream->info.info.flac.channels = channels;
	}
	else
		return -EINVAL;

	spa_dll_set_bw(&stream->dll, SPA_DLL_BW_MIN, 1000, sample_rate);

	return 0;
}

/* {"player":{}} */
static int handle_stream_start(struct stream *stream, struct spa_json *payload)
{
	struct impl *impl = stream->impl;
	struct spa_json it[1];
	char key[256];
        const char *v;
	int l;

	while ((l = spa_json_object_next(payload, key, sizeof(key), &v)) > 0) {
		if (spa_streq(key, "player")) {
			if (!spa_json_is_object(v, l))
				return -EPROTO;
			spa_json_enter(payload, &it[0]);
			parse_player(stream, &it[0]);
		}
	}

	if (stream->stream == NULL) {
		create_stream(stream);

		pw_timer_queue_cancel(&stream->timer);
		pw_timer_queue_add(impl->timer_queue, &stream->timer,
			NULL, 0, do_stream_timer, stream);
	} else {
	}

	return 0;
}

static void stream_clear(struct stream *stream)
{
	spa_ringbuffer_init(&stream->ring);
	memset(stream->buffer, 0, stream->buffer_size);
}

static int handle_stream_clear(struct stream *stream, struct spa_json *payload)
{
	stream_clear(stream);
	return 0;
}
static int handle_stream_end(struct stream *stream, struct spa_json *payload)
{
	if (stream->stream != NULL) {
		pw_stream_destroy(stream->stream);
		stream->stream = NULL;
		stream_clear(stream);
	}
	return 0;
}

static int handle_group_update(struct stream *stream, struct spa_json *payload)
{
	return 0;
}

/* { "type":... "payload":{...} } */
static int do_parse_text(struct stream *stream, const char *content, int size)
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
		res = handle_server_hello(stream, payload);
	else if (spa_streq(type, "server/state"))
		res = handle_server_state(stream, payload);
	else if (spa_streq(type, "server/time"))
		res = handle_server_time(stream, payload);
	else if (spa_streq(type, "server/command"))
		res = handle_server_command(stream, payload);
	else if (spa_streq(type, "stream/start"))
		res = handle_stream_start(stream, payload);
	else if (spa_streq(type, "stream/end"))
		res = handle_stream_end(stream, payload);
	else if (spa_streq(type, "stream/clear"))
		res = handle_stream_clear(stream, payload);
	else if (spa_streq(type, "group/update"))
		res = handle_group_update(stream, payload);
	else
		res = 0;

	return res;
}

static int do_handle_binary(struct stream *stream, const uint8_t *payload, int size)
{
	struct impl *impl = stream->impl;
	int32_t filled;
	uint32_t index, length = size - 9;
	uint64_t timestamp;

	if (payload[0] != 4 || stream->stream == NULL)
		return 0;

	timestamp  = ((uint64_t)payload[1]) << 56;
	timestamp |= ((uint64_t)payload[2]) << 48;
	timestamp |= ((uint64_t)payload[3]) << 40;
	timestamp |= ((uint64_t)payload[4]) << 32;
	timestamp |= ((uint64_t)payload[5]) << 24;
	timestamp |= ((uint64_t)payload[6]) << 16;
	timestamp |= ((uint64_t)payload[7]) <<  8;
	timestamp |= ((uint64_t)payload[8]);

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);
	if (filled < 0) {
		pw_log_warn("%p: underrun write:%u filled:%d",
				stream, index, filled);
	} else if (filled + length > stream->buffer_size) {
		pw_log_debug("%p: overrun write:%u filled:%d",
				stream, index, filled);
	}

	spa_ringbuffer_write_data(&stream->ring,
			stream->buffer, stream->buffer_size,
			index % stream->buffer_size,
			&payload[9], length);

	spa_ringbuffer_write_update(&stream->ring, index + length);

	pw_loop_lock(impl->data_loop);
	spa_regress_update(&stream->regress_index, index, timestamp);
	pw_loop_unlock(impl->data_loop);

	return 0;
}

static void on_connection_message(void *data, int opcode, void *payload, size_t size)
{
	struct stream *stream = data;
	if (opcode == PW_WEBSOCKET_OPCODE_TEXT) {
		do_parse_text(stream, payload, size);
	} else if (opcode == PW_WEBSOCKET_OPCODE_BINARY) {
		do_handle_binary(stream, payload, size);
	} else {
		pw_log_warn("%02x unknown %08x", opcode, (int)size);
	}
}

static void stream_destroy(struct stream *stream)
{
	handle_stream_end(stream, NULL);
	if (stream->conn) {
		spa_hook_remove(&stream->conn_listener);
		pw_websocket_connection_destroy(stream->conn);
	}
	pw_timer_queue_cancel(&stream->timer);
	spa_list_remove(&stream->link);
	free(stream->buffer);
	free(stream);
}

static void on_connection_destroy(void *data)
{
	struct stream *stream = data;
	stream->conn = NULL;
	pw_log_info("connection %p destroy", stream);
}
static void on_connection_error(void *data, int res, const char *reason)
{
	struct stream *stream = data;
	pw_log_error("connection %p error %d %s", stream, res, reason);
}

static void on_connection_disconnected(void *data)
{
	struct stream *stream = data;
	stream_destroy(stream);
}

static const struct pw_websocket_connection_events websocket_connection_events = {
	PW_VERSION_WEBSOCKET_CONNECTION_EVENTS,
	.destroy = on_connection_destroy,
	.error = on_connection_error,
	.disconnected = on_connection_disconnected,
	.message = on_connection_message,
};

static struct stream *stream_new(struct impl *impl, struct pw_websocket_connection *conn)
{
	struct stream *stream;

	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return NULL;

	stream->impl = impl;
	spa_list_append(&impl->streams, &stream->link);

	stream->conn = conn;
	pw_websocket_connection_add_listener(stream->conn, &stream->conn_listener,
			&websocket_connection_events, stream);

	spa_regress_init(&stream->regress_index, 5);
	spa_regress_init(&stream->regress_time, 5);

	spa_dll_init(&stream->dll);
	stream->resync = true;

	return stream;
}

static void on_websocket_connected(void *data, void *user,
		struct pw_websocket_connection *conn, const char *path)
{
	struct impl *impl = data;
	struct stream *stream;
	pw_log_info("connected to %s", path);
	stream = stream_new(impl, conn);
	send_client_hello(stream);
}

static const struct pw_websocket_events websocket_events = {
	PW_VERSION_WEBSOCKET_EVENTS,
	.connected = on_websocket_connected,
};

static void on_zeroconf_added(void *data, void *user, const struct spa_dict *info)
{
}

static void on_zeroconf_removed(void *data, void *user, const struct spa_dict *info)
{
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
	struct stream *s;

	spa_list_consume(s, &impl->streams, link)
		stream_destroy(s);

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
	spa_list_init(&impl->streams);

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
	if (hostname != NULL) {
		port = pw_properties_get(props, "sendspin.port");
		if (port == NULL)
			port = SPA_STRINGIFY(DEFAULT_SERVER_PORT);
		if ((path = pw_properties_get(props, "sendspin.path")) == NULL)
			path = DEFAULT_SENDSPIN_PATH;

		pw_websocket_connect(impl->websocket, NULL, hostname, port, path);
	} else {
		if ((hostname = pw_properties_get(props, "source.ip")) == NULL)
			hostname = DEFAULT_SOURCE_IP;
		if ((port = pw_properties_get(props, "source.port")) == NULL)
			port = SPA_STRINGIFY(DEFAULT_SOURCE_PORT);
		if ((path = pw_properties_get(props, "source.path")) == NULL)
			path = DEFAULT_SOURCE_PATH;

		pw_websocket_listen(impl->websocket, NULL, hostname, port, path);

		if (impl->zeroconf) {
			str = pw_properties_get(props, "sendspin.client-id");
			pw_zeroconf_set_announce(impl->zeroconf, NULL,
				&SPA_DICT_ITEMS(
					SPA_DICT_ITEM("zeroconf.service", PW_SENDSPIN_CLIENT_SERVICE),
					SPA_DICT_ITEM("zeroconf.session", str),
					SPA_DICT_ITEM("zeroconf.port", port),
					SPA_DICT_ITEM("path", path)));
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
