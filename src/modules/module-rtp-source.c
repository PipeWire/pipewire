/* PipeWire
 *
 * Copyright © 2022 Wim Taymans <wim.taymans@gmail.com>
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
#include <spa/utils/dll.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/sap.h>
#include <module-rtp/rtp.h>

#ifdef __FreeBSD__
#define ifr_ifindex ifr_index
#endif

/** \page page_module_rtp_source PipeWire Module: RTP source
 *
 * The `rtp-source` module creates a PipeWire source that receives audio
 * RTP packets.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `sap.ip = <str>`: IP address of the SAP messages, default "224.0.0.56"
 * - `sap.port = <str>`: port of the SAP messages, default 9875
 * - `local.ifname = <str>`: interface name to use
 * - `sess.latency.msec = <str>`: target network latency in milliseconds, default 100
 * - `stream.props = {}`: properties to be passed to the stream
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_MEDIA_NAME
 * - \ref PW_KEY_MEDIA_CLASS
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-rtp-source
 *     args = {
 *         #sap.ip = 224.0.0.56
 *         #sap.port = 9875
 *         #local.ifname = eth0
 *         sess.latency.msec = 100
 *         #node.always-process = false # true to receive even when not running
 *         stream.props = {
 *            #media.class = "Audio/Source"
 *            #node.name = "rtp-source"
 *         }
 *         stream.rules = [
 *             {   matches = [
 *                     # any of the items in matches needs to match, if one does,
 *                     # actions are emited.
 *                     {   # all keys must match the value. ~ in value starts regex.
 *                         #rtp.origin = "wim 3883629975 0 IN IP4 0.0.0.0"
 *                         #rtp.payload = "127"
 *                         #rtp.fmt = "L16/48000/2"
 *                         #rtp.session = "PipeWire RTP Stream on fedora"
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         #sess.latency.msec = 100
 *                         #target.object = ""
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * \since 0.3.60
 */

#define NAME "rtp-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define SAP_MIME_TYPE			"application/sdp"

#define ERROR_MSEC			2
#define MAX_SESSIONS			16

#define DEFAULT_CLEANUP_INTERVAL_SEC	90
#define DEFAULT_SAP_IP			"224.0.0.56"
#define DEFAULT_SAP_PORT		9875
#define DEFAULT_SESS_LATENCY		100

#define BUFFER_SIZE			(1u<<22)
#define BUFFER_MASK			(BUFFER_SIZE-1)

#define USAGE	"sap.ip=<SAP IP address to listen on, default "DEFAULT_SAP_IP"> "				\
		"sap.port=<SAP port to listen on, default "SPA_STRINGIFY(DEFAULT_SAP_PORT)"> "			\
		"local.ifname=<local interface name to use> "							\
		"sess.latency.msec=<target network latency, default "SPA_STRINGIFY(DEFAULT_SESS_LATENCY)"> "	\
		"stream.props= { key=value ... } "								\
		"stream.rules=<rules> "

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "RTP Source" },
	{ PW_KEY_MODULE_USAGE,	USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *module_context;

	struct pw_loop *loop;
	struct pw_loop *data_loop;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct spa_source *timer;
	struct spa_source *sap_source;

	struct pw_properties *stream_props;

	unsigned int do_disconnect:1;

	char *ifname;
	char *sap_ip;
	bool always_process;
	int sap_port;
	int sess_latency_msec;
	uint32_t cleanup_interval;

	struct spa_list sessions;
	uint32_t n_sessions;
};

static const struct format_info {
	uint32_t format;
	uint32_t size;
	const char *mime;
} format_info[] = {
	{ SPA_AUDIO_FORMAT_U8, 1, "L8" },
	{ SPA_AUDIO_FORMAT_ALAW, 1, "PCMA" },
	{ SPA_AUDIO_FORMAT_ULAW, 1, "PCMU" },
	{ SPA_AUDIO_FORMAT_S16_BE, 2, "L16" },
	{ SPA_AUDIO_FORMAT_S24_BE, 3, "L24" },
};

static const struct format_info *find_format_info(const char *mime)
{
	SPA_FOR_EACH_ELEMENT_VAR(format_info, f)
		if (spa_streq(f->mime, mime))
			return f;
	return NULL;
}

struct sdp_info {
	uint16_t hash;

	char origin[128];
	char session[256];
	char channelmap[512];

	struct sockaddr_storage sa;
	socklen_t salen;

	uint16_t port;
	uint8_t payload;

	const struct format_info *format_info;
	struct spa_audio_info_raw info;
	uint32_t stride;
};

struct session {
	struct impl *impl;
	struct spa_list link;

	uint64_t timestamp;

	struct sdp_info info;

	struct spa_source *source;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	uint32_t expected_ssrc;
	uint16_t expected_seq;
	unsigned have_ssrc:1;
	unsigned have_seq:1;
	unsigned have_sync:1;

	struct spa_ringbuffer ring;
	uint8_t buffer[BUFFER_SIZE];

	struct spa_io_rate_match *rate_match;
	struct spa_dll dll;
	uint32_t target_buffer;
	uint32_t last_packet_size;
	float max_error;
	unsigned buffering:1;
	unsigned first:1;
};

static void stream_destroy(void *d)
{
	struct session *sess = d;
	spa_hook_remove(&sess->stream_listener);
	sess->stream = NULL;
}

static void stream_process(void *data)
{
	struct session *sess = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t index, target_buffer;
	int32_t avail, wanted;

	if ((buf = pw_stream_dequeue_buffer(sess->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	wanted = buf->requested ?
		SPA_MIN(buf->requested * sess->info.stride, d[0].maxsize)
		: d[0].maxsize;

	avail = spa_ringbuffer_get_read_index(&sess->ring, &index);

	target_buffer = sess->target_buffer + sess->last_packet_size / 2;

	if (avail < wanted || sess->buffering) {
		memset(d[0].data, 0, wanted);
		if (!sess->buffering && sess->have_sync) {
			pw_log_debug("underrun %u/%u < %u, buffering...",
					avail, target_buffer, wanted);
			sess->buffering = true;
		}
	} else {
		float error, corr;
		if (avail > (int32_t)SPA_MIN(target_buffer * 8, BUFFER_SIZE)) {
			pw_log_warn("overrun %u > %u", avail, target_buffer * 8);
			index += avail - target_buffer;
			avail = target_buffer;
		} else {
			if (sess->first) {
				if ((uint32_t)avail > target_buffer) {
					uint32_t skip = avail - target_buffer;
					pw_log_debug("first: avail:%d skip:%u target:%u",
							avail, skip, target_buffer);
					index += skip;
					avail = target_buffer;
				}
				sess->first = false;
			}
			error = (float)target_buffer - (float)avail;
			error = SPA_CLAMP(error, -sess->max_error, sess->max_error);

			corr = spa_dll_update(&sess->dll, error);

			pw_log_debug("avail:%u target:%u error:%f corr:%f", avail,
					target_buffer, error, corr);

			if (sess->rate_match) {
				SPA_FLAG_SET(sess->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE);
				sess->rate_match->rate = 1.0f / corr;
			}
		}
		spa_ringbuffer_read_data(&sess->ring,
				sess->buffer,
				BUFFER_SIZE,
				index & BUFFER_MASK,
				d[0].data, wanted);

		index += wanted;
		spa_ringbuffer_read_update(&sess->ring, index);
	}
	d[0].chunk->size = wanted;
	d[0].chunk->stride = sess->info.stride;
	d[0].chunk->offset = 0;
	buf->size = wanted / sess->info.stride;

	pw_stream_queue_buffer(sess->stream, buf);
}

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct session *sess = data;
	switch (id) {
	case SPA_IO_RateMatch:
		sess->rate_match = area;
		break;
	}
}

static void session_touch(struct session *sess)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	sess->timestamp = SPA_TIMESPEC_TO_NSEC(&ts);
}

static void
on_rtp_io(void *data, int fd, uint32_t mask)
{
	struct session *sess = data;
	struct rtp_header *hdr;
	ssize_t len, hlen;
	uint8_t buffer[2048], *payload;

	if (mask & SPA_IO_IN) {
		uint32_t index, expected_index, timestamp;
		uint16_t seq;
		int32_t filled;

		if ((len = recv(fd, buffer, sizeof(buffer), 0)) < 0)
			goto receive_error;

		if (len < 12)
			goto short_packet;

		hdr = (struct rtp_header*)buffer;
		if (hdr->v != 2)
			goto invalid_version;

		hlen = 12 + hdr->cc * 4;
		if (hlen > len)
			goto invalid_len;

		if (sess->have_ssrc && sess->expected_ssrc != hdr->ssrc)
			goto unexpected_ssrc;
		sess->expected_ssrc = hdr->ssrc;
		sess->have_ssrc = true;

		seq = ntohs(hdr->sequence_number);
		if (sess->have_seq && sess->expected_seq != seq) {
			pw_log_info("unexpected seq (%d != %d)", seq, sess->expected_seq);
			sess->have_sync = false;
		}
		sess->expected_seq = seq + 1;
		sess->have_seq = true;

		len = SPA_ROUND_DOWN(len - hlen, sess->info.stride);
		payload = &buffer[hlen];

		filled = spa_ringbuffer_get_write_index(&sess->ring, &index);

		timestamp = ntohl(hdr->timestamp);
		expected_index = timestamp * sess->info.stride;

		if (!sess->have_sync) {
			pw_log_trace("got rtp, no sync");
			sess->ring.readindex = sess->ring.writeindex =
				index = expected_index;
			filled = 0;
			sess->have_sync = true;
			sess->buffering = true;
			pw_log_debug("sync to timestamp %u", index);

			spa_dll_init(&sess->dll);
			spa_dll_set_bw(&sess->dll, SPA_DLL_BW_MIN, 128, sess->info.info.rate);

		} else if (expected_index != index) {
			pw_log_trace("got rtp, wrong timestamp");
			pw_log_debug("unexpected timestamp (%u != %u)",
					index / sess->info.stride,
					expected_index / sess->info.stride);
			index = expected_index;
			filled = 0;
		}

		if (filled + len > BUFFER_SIZE) {
			pw_log_debug("got rtp, capture overrun %u %zd", filled, len);
			sess->have_sync = false;
		} else {
			uint32_t target_buffer;

			pw_log_trace("got rtp packet len:%zd", len);
			spa_ringbuffer_write_data(&sess->ring,
					sess->buffer,
					BUFFER_SIZE,
					index & BUFFER_MASK,
					payload, len);
			index += len;
			filled += len;
			spa_ringbuffer_write_update(&sess->ring, index);

			sess->last_packet_size = len;
			target_buffer = sess->target_buffer + len/2;

			if (sess->buffering && (uint32_t)filled > target_buffer) {
				sess->buffering = false;
				pw_log_debug("buffering done %u > %u",
					filled, target_buffer);
			}
		}
	}
	session_touch(sess);
	return;

receive_error:
	pw_log_warn("recv error: %m");
	return;
short_packet:
	pw_log_warn("short packet received");
	return;
invalid_version:
	pw_log_warn("invalid RTP version");
	return;
invalid_len:
	pw_log_warn("invalid RTP length");
	return;
unexpected_ssrc:
	pw_log_warn("unexpected SSRC (expected %u != %u)",
		sess->expected_ssrc, hdr->ssrc);
	return;
}

static int make_socket(const struct sockaddr* sa, socklen_t salen, char *ifname)
{
	int af, fd, val, res;
	struct ifreq req;

	af = sa->sa_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
#ifdef SO_TIMESTAMP
	val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &val, sizeof(val)) < 0) {
		res = -errno;
		pw_log_error("setsockopt failed: %m");
		goto error;
	}
#endif
	val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		res = -errno;
		pw_log_error("setsockopt failed: %m");
		goto error;
	}

	spa_zero(req);
	if (ifname) {
		snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
		res = ioctl(fd, SIOCGIFINDEX, &req);
	        if (res < 0)
	                pw_log_warn("SIOCGIFINDEX %s failed: %m", ifname);
	}
	res = 0;
	if (af == AF_INET) {
		static const uint32_t ipv4_mcast_mask = 0xe0000000;
		struct sockaddr_in *sa4 = (struct sockaddr_in*)sa;
		if ((ntohl(sa4->sin_addr.s_addr) & ipv4_mcast_mask) == ipv4_mcast_mask) {
			struct ip_mreqn mr4;
			memset(&mr4, 0, sizeof(mr4));
			mr4.imr_multiaddr = sa4->sin_addr;
			mr4.imr_ifindex = req.ifr_ifindex;
			res = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr4, sizeof(mr4));
		} else {
			sa4->sin_addr.s_addr = INADDR_ANY;
		}
	} else if (af == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)sa;
		if (sa6->sin6_addr.s6_addr[0] == 0xff) {
			struct ipv6_mreq mr6;
			memset(&mr6, 0, sizeof(mr6));
			mr6.ipv6mr_multiaddr = sa6->sin6_addr;
			mr6.ipv6mr_interface = req.ifr_ifindex;
			res = setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mr6, sizeof(mr6));
		} else {
		        sa6->sin6_addr = in6addr_any;
		}
	} else {
		res = -EINVAL;
		goto error;
	}

	if (res < 0) {
		res = -errno;
		pw_log_error("join mcast failed: %m");
		goto error;
	}

	if (bind(fd, sa, salen) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	return fd;
error:
	return res;
}

static uint32_t msec_to_bytes(struct sdp_info *info, uint32_t msec)
{
	return msec * info->stride * info->info.rate / 1000;
}

static void session_free(struct session *sess)
{
	if (sess->impl) {
		pw_log_info("free session %s %s", sess->info.origin, sess->info.session);
		sess->impl->n_sessions--;
		spa_list_remove(&sess->link);
	}
	if (sess->stream)
		pw_stream_destroy(sess->stream);
	if (sess->source)
		pw_loop_destroy_source(sess->impl->data_loop, sess->source);
	free(sess);
}

struct session_info {
	struct session *session;
	struct pw_properties *props;
	bool matched;
};

static int rule_matched(void *data, const char *location, const char *action,
                        const char *str, size_t len)
{
	struct session_info *i = data;
	int res = 0;

	i->matched = true;
	if (spa_streq(action, "create-stream")) {
		pw_properties_update_string(i->props, str, len);
	}
	return res;
}

static int session_start(struct impl *impl, struct session *session) {
	int fd;
	if (session->source)
	  return 0;

	pw_log_info("starting RTP listener");

	if ((fd = make_socket((const struct sockaddr *)&session->info.sa,
					session->info.salen, impl->ifname)) < 0) {
		pw_log_error("failed to create socket: %m");
		return fd;
	}

	session->source = pw_loop_add_io(impl->data_loop, fd,
				SPA_IO_IN, true, on_rtp_io, session);
	if (session->source == NULL) {
		pw_log_error("can't create io source: %m");
		close(fd);
		return -errno;
	}
	return 0;
}

static void session_stop(struct impl *impl, struct session *session) {
	if (!session->source)
		return;

	pw_log_info("stopping RTP listener");

	pw_loop_destroy_source(
		session->impl->data_loop,
		session->source
	);

	session->source = NULL;
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct session *sess = d;
	struct impl *impl = sess->impl;

	switch (state) {
		case PW_STREAM_STATE_UNCONNECTED:
			pw_log_info("stream disconnected, unloading");
			pw_impl_module_schedule_destroy(impl->module);
			break;
		case PW_STREAM_STATE_ERROR:
			pw_log_error("stream error: %s", error);
			break;
		case PW_STREAM_STATE_STREAMING:
			if ((errno = -session_start(impl, sess)) < 0)
				pw_log_error("failed to start RTP stream: %m");
			break;
		case PW_STREAM_STATE_PAUSED:
			if (!impl->always_process)
				session_stop(impl, sess);
		  break;
		default:
			break;
	}
}

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.io_changed = stream_io_changed,
	.process = stream_process
};

static int session_new(struct impl *impl, struct sdp_info *info)
{
	struct session *session;
	const struct spa_pod *params[1];
	struct spa_pod_builder b;
	uint32_t n_params;
	uint8_t buffer[1024];
	struct pw_properties *props;
	int res, sess_latency_msec;
	const char *str;

	if (impl->n_sessions >= MAX_SESSIONS) {
		pw_log_warn("too many sessions (%u >= %u)", impl->n_sessions, MAX_SESSIONS);
		return -EMFILE;
	}

	session = calloc(1, sizeof(struct session));
	if (session == NULL)
		return -errno;

	session->info = *info;
	session->first = true;

	props = pw_properties_copy(impl->stream_props);
	if (props == NULL) {
		res = -errno;
		goto error;
	}

	pw_properties_set(props, "rtp.origin", info->origin);
	pw_properties_setf(props, "rtp.payload", "%u", info->payload);
	pw_properties_setf(props, "rtp.fmt", "%s/%u/%u", info->format_info->mime,
			info->info.rate, info->info.channels);
	if (info->session[0]) {
		pw_properties_set(props, "rtp.session", info->session);
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "RTP Stream (%s)",
				info->session);
		pw_properties_setf(props, PW_KEY_NODE_NAME, "%s",
				info->session);
	} else {
		pw_properties_set(props, PW_KEY_MEDIA_NAME, "RTP Stream");
	}

	if ((str = pw_properties_get(impl->props, "stream.rules")) != NULL) {
		struct session_info sinfo = {
			.session = session,
			.props = props,
		};
		pw_conf_match_rules(str, strlen(str), NAME, &props->dict,
				rule_matched, &sinfo);

		if (!sinfo.matched) {
			res = 0;
			pw_log_info("session '%s' was not matched", info->session);
			goto error;
		}
	}

	pw_log_info("new session %s %s", info->origin, info->session);

	sess_latency_msec = pw_properties_get_uint32(props,
			"sess.latency.msec", impl->sess_latency_msec);

	session->target_buffer = msec_to_bytes(info, sess_latency_msec);
	session->max_error = msec_to_bytes(info, ERROR_MSEC);

	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", info->info.rate);
	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
			session->target_buffer / (2 * info->stride), info->info.rate);

	spa_dll_init(&session->dll);
	spa_dll_set_bw(&session->dll, SPA_DLL_BW_MIN, 128, session->info.info.rate);

	if (info->channelmap[0]) {
		pw_properties_set(props, PW_KEY_NODE_CHANNELNAMES, info->channelmap);
		pw_log_info("channelmap: %s", info->channelmap);
	}

	session->stream = pw_stream_new(impl->core,
			"rtp-source playback", props);
	if (session->stream == NULL) {
		res = -errno;
		pw_log_error("can't create stream: %m");
		goto error;
	}

	pw_stream_add_listener(session->stream,
			&session->stream_listener,
			&out_stream_events, session);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&info->info);

	if ((res = pw_stream_connect(session->stream,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0) {
		pw_log_error("can't connect stream: %s", spa_strerror(res));
		goto error;
	}

	if (impl->always_process &&
		(res = session_start(impl, session)) < 0)
		goto error;

	session_touch(session);

	session->impl = impl;
	spa_list_append(&impl->sessions, &session->link);
	impl->n_sessions++;

	return 0;
error:
	session_free(session);
	return res;
}

static struct session *session_find(struct impl *impl, struct sdp_info *info)
{
	struct session *sess;
	spa_list_for_each(sess, &impl->sessions, link) {
		if (info->hash == sess->info.hash &&
		    spa_streq(info->origin, sess->info.origin))
			return sess;
	}
	return NULL;
}

static int parse_sdp_c(struct impl *impl, char *c, struct sdp_info *info)
{
	int res;

	c[strcspn(c, "/")] = 0;
	if (spa_strstartswith(c, "c=IN IP4 ")) {
		struct sockaddr_in *sa = (struct sockaddr_in*) &info->sa;

		c += strlen("c=IN IP4 ");
		if (inet_pton(AF_INET, c, &sa->sin_addr) <= 0) {
			res = -errno;
			pw_log_warn("inet_pton(%s) failed: %m", c);
			goto error;
		}
		sa->sin_family = AF_INET;
		info->salen = sizeof(struct sockaddr_in);
	}
	else if (spa_strstartswith(c, "c=IN IP6 ")) {
		struct sockaddr_in6 *sa = (struct sockaddr_in6*) &info->sa;

		c += strlen("c=IN IP6 ");
		if (inet_pton(AF_INET6, c, &sa->sin6_addr) <= 0) {
			res = -errno;
			pw_log_warn("inet_pton(%s) failed: %m", c);
			goto error;
		}

		sa->sin6_family = AF_INET6;
		info->salen = sizeof(struct sockaddr_in6);
	} else
		return -EINVAL;


	res= 0;
error:
	return res;
}

static int parse_sdp_m(struct impl *impl, char *c, struct sdp_info *info)
{
	int port, payload;

	if (!spa_strstartswith(c, "m=audio "))
		return -EINVAL;

	c += strlen("m=audio ");
	if (sscanf(c, "%i RTP/AVP %i", &port, &payload) != 2)
		return -EINVAL;

	if (port <= 0 || port > 0xFFFF)
		return -EINVAL;

	if (payload < 0 || payload > 127)
		return -EINVAL;

	info->port = (uint16_t) port;
	info->payload = (uint8_t) payload;

	return 0;
}

// some AES67 devices have channelmap encoded in i=*
// if `i` record is found, it matches the template
// and channel count matches, name the channels respectively
// `i=2 channels: 01, 08` is the format
static int parse_sdp_i(struct impl *impl, char *c, struct sdp_info *info)
{
	if (!strstr(c, " channels: ")) {
		return 0;
	}

	c += strlen("i=");
	c[strcspn(c, " ")] = '\0';

	uint32_t channels;
	if (sscanf(c, "%u", &channels) != 1 || channels <= 0 || channels > SPA_AUDIO_MAX_CHANNELS)
		return 0;

	c += strcspn(c, "\0");
	c += strlen(" channels: ");

	strncpy(info->channelmap, c, sizeof(info->channelmap) - 1);

	return 0;
}

static int parse_sdp_a(struct impl *impl, char *c, struct sdp_info *info)
{
	int payload, len, rate, channels;

	if (!spa_strstartswith(c, "a=rtpmap:"))
		return 0;

	c += strlen("a=rtpmap:");

	if (sscanf(c, "%i %n", &payload, &len) != 1)
		return -EINVAL;

	if (payload < 0 || payload > 127)
		return -EINVAL;

	if (payload != info->payload)
		return 0;

	c += len;
	c[strcspn(c, "/")] = 0;

	info->format_info = find_format_info(c);
	if (info->format_info == NULL)
		return -EINVAL;

	info->info.format = info->format_info->format;
	info->stride = info->format_info->size;

	c += strlen(c) + 1;
	if (sscanf(c, "%u/%u", &rate, &channels) == 2) {
		info->info.rate = rate;
		info->info.channels = channels;
		pw_log_info("rate: %d, ch: %d", rate, channels);
		if (channels == 2) {
			info->info.position[0] = SPA_AUDIO_CHANNEL_FL;
			info->info.position[1] = SPA_AUDIO_CHANNEL_FR;
		}
	} else if (sscanf(c, "%u", &rate) == 1) {
		info->info.rate = rate;
		info->info.channels = 1;
	} else
		return -EINVAL;

	info->stride *= info->info.channels;

	return 0;
}

static int parse_sdp(struct impl *impl, char *sdp, struct sdp_info *info)
{
	char *s = sdp;
	int count = 0, res = 0;
	size_t l;

	while (*s) {
		if ((l = strcspn(s, "\r\n")) < 2)
			goto too_short;

		s[l] = 0;
		pw_log_debug("%d: %s", count, s);

		if (count++ == 0 && strcmp(s, "v=0") != 0)
			goto invalid_version;

		if (spa_strstartswith(s, "o="))
			snprintf(info->origin, sizeof(info->origin), "%s", &s[2]);
		else if (spa_strstartswith(s, "s="))
			snprintf(info->session, sizeof(info->session), "%s", &s[2]);
		else if (spa_strstartswith(s, "c="))
			res = parse_sdp_c(impl, s, info);
		else if (spa_strstartswith(s, "m="))
			res = parse_sdp_m(impl, s, info);
		else if (spa_strstartswith(s, "a="))
			res = parse_sdp_a(impl, s, info);
		else if (spa_strstartswith(s, "i="))
			res = parse_sdp_i(impl, s, info);

		if (res < 0)
			goto error;
		s += l + 1;
		while (isspace(*s))
			s++;
        }
	if (((struct sockaddr*) &info->sa)->sa_family == AF_INET)
		((struct sockaddr_in*) &info->sa)->sin_port = htons(info->port);
	else
		((struct sockaddr_in6*) &info->sa)->sin6_port = htons(info->port);

	return 0;
too_short:
	pw_log_warn("SDP: line starting with `%.6s...' too short", s);
	return -EINVAL;
invalid_version:
	pw_log_warn("SDP: invalid first version line `%*s'", (int)l, s);
	return -EINVAL;
error:
	pw_log_warn("SDP: error: %s", spa_strerror(res));
	return res;
}

static int parse_sap(struct impl *impl, void *data, size_t len)
{
	struct sap_header *header;
	char *mime, *sdp;
	struct sdp_info info;
	struct session *sess;
	int res;
	size_t offs;
	bool bye;

	if (len < 8)
		return -EINVAL;

	header = (struct sap_header*) data;
	if (header->v != 1)
		return -EINVAL;

	if (header->e)
		return -ENOTSUP;
	if (header->c)
		return -ENOTSUP;

	offs = header->a ? 12 : 8;
	offs += header->auth_len * 4;
	if (len <= offs)
		return -EINVAL;

	mime = SPA_PTROFF(data, offs, char);
	if (spa_strstartswith(mime, "v=0")) {
		sdp = mime;
		mime = SAP_MIME_TYPE;
	} else if (spa_streq(mime, SAP_MIME_TYPE))
		sdp = SPA_PTROFF(mime, strlen(mime)+1, char);
	else
		return -EINVAL;

	pw_log_debug("got sap: %s %s", mime, sdp);

	spa_zero(info);
	if ((res = parse_sdp(impl, sdp, &info)) < 0)
		return res;

	bye = header->t;

	sess = session_find(impl, &info);
	if (sess == NULL) {
		if (!bye)
			session_new(impl, &info);
	} else {
		if (bye)
			session_free(sess);
		else
			session_touch(sess);
	}
	return res;
}

static void
on_sap_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;

	if (mask & SPA_IO_IN) {
		uint8_t buffer[2048];
		ssize_t len;

		if ((len = recv(fd, buffer, sizeof(buffer), 0)) < 0) {
			pw_log_warn("recv error: %m");
			return;
		}
		if ((size_t)len >= sizeof(buffer))
			return;

		buffer[len] = 0;
		parse_sap(impl, buffer, len);
	}
}

static int start_sap_listener(struct impl *impl)
{
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	struct sockaddr *sa;
	socklen_t salen;
	int fd, res;

	if (inet_pton(AF_INET, impl->sap_ip, &sa4.sin_addr) > 0) {
		sa4.sin_family = AF_INET;
		sa4.sin_port = htons(impl->sap_port);
		sa = (struct sockaddr*) &sa4;
		salen = sizeof(sa4);
	} else if (inet_pton(AF_INET6, impl->sap_ip, &sa6.sin6_addr) > 0) {
		sa6.sin6_family = AF_INET6;
		sa6.sin6_port = htons(impl->sap_port);
		sa = (struct sockaddr*) &sa6;
		salen = sizeof(sa6);
	} else
		return -EINVAL;

	if ((fd = make_socket(sa, salen, impl->ifname)) < 0)
		return fd;

	pw_log_info("starting SAP listener");
	impl->sap_source = pw_loop_add_io(impl->loop, fd,
				SPA_IO_IN, true, on_sap_io, impl);
	if (impl->sap_source == NULL) {
		res = -errno;
		goto error;
	}
	return 0;
error:
	close(fd);
	return res;

}

static void on_timer_event(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	struct timespec now;
	struct session *sess, *tmp;
	uint64_t timestamp, interval;

	clock_gettime(CLOCK_MONOTONIC, &now);
	timestamp = SPA_TIMESPEC_TO_NSEC(&now);
	interval = impl->cleanup_interval * SPA_NSEC_PER_SEC;

	spa_list_for_each_safe(sess, tmp, &impl->sessions, link) {
		if (sess->timestamp + interval < timestamp) {
			pw_log_debug("More than %lu elapsed from last advertisement at %lu", interval, sess->timestamp);
			pw_log_info("No advertisement packets found for timeout, closing RTP source");
			session_free(sess);
		}
	}
}

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
	struct session *sess;
	spa_list_consume(sess, &impl->sessions, link)
		session_free(sess);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->sap_source)
		pw_loop_destroy_source(impl->loop, impl->sap_source);
	if (impl->timer)
		pw_loop_destroy_source(impl->loop, impl->timer);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->ifname);
	free(impl->sap_ip);
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

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	const char *str;
	struct timespec value, interval;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	spa_list_init(&impl->sessions);

	if (args == NULL)
		args = "";

	impl->props = pw_properties_new_string(args);
	impl->stream_props = pw_properties_new(NULL, NULL);
	if (impl->props == NULL || impl->stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}

	impl->module = module;
	impl->module_context = context;
	impl->loop = pw_context_get_main_loop(context);
	impl->data_loop = pw_data_loop_get_loop(pw_context_get_data_loop(context));

	if (pw_properties_get(impl->stream_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(impl->stream_props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(impl->stream_props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(impl->stream_props, PW_KEY_NODE_NETWORK, "true");

	if ((str = pw_properties_get(impl->props, "stream.props")) != NULL)
		pw_properties_update_string(impl->stream_props, str, strlen(str));

	str = pw_properties_get(impl->props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	impl->always_process = pw_properties_get_bool(impl->props, PW_KEY_NODE_ALWAYS_PROCESS, false);

	str = pw_properties_get(impl->props, "sap.ip");
	impl->sap_ip = strdup(str ? str : DEFAULT_SAP_IP);
	impl->sap_port = pw_properties_get_uint32(impl->props,
			"sap.port", DEFAULT_SAP_PORT);
	impl->sess_latency_msec = pw_properties_get_uint32(impl->props,
			"sess.latency.msec", DEFAULT_SESS_LATENCY);
	impl->cleanup_interval = pw_properties_get_uint32(impl->props,
			"sap.interval.sec", DEFAULT_CLEANUP_INTERVAL_SEC);

	impl->core = pw_context_get_object(impl->module_context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(impl->props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->module_context,
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

	impl->timer = pw_loop_add_timer(impl->loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto out;
	}
	value.tv_sec = 0;
	value.tv_nsec = 1;
	interval.tv_sec = impl->cleanup_interval;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->loop, impl->timer, &value, &interval, false);

	if ((res = start_sap_listener(impl)) < 0)
		goto out;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-rtp-source");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
