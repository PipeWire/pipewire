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
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ctype.h>

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>

#include <module-rtp/sap.h>
#include <module-rtp/rtp.h>


/** \page page_module_rtp_source PipeWire Module: RTP source
 *
 * The `rtp-source` module creates a PipeWire source that receives audio
 * RTP packets.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `source.props = {}`: properties to be passed to the source stream
 * - `source.name = <str>`: node.name of the source
 * - `local.ip = <str>`: local sender ip
 * - `sess.latency.msec = <str>`: target network latency in milliseconds
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_MEDIA_NAME
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-module-rtp-source
 *      args = {
 *          local.ip = 224.0.0.56
 *          sess.latency.msec = 200
 *          source.name = "RTP Source"
 *          source.props = {
 *             node.name = "rtp-source"
 *          }
 *      }
 *  }
 *]
 *\endcode
 *
 */

#define NAME "rtp-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define SAP_DEFAULT_IP		"224.0.0.56"
#define SAP_DEFAULT_PORT	9875
#define DEFAULT_SESS_LATENCY	200

struct impl {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *module_context;

	struct pw_loop *loop;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct spa_source *sap_source;

	struct pw_stream *playback;
	struct spa_hook playback_listener;
	struct pw_properties *playback_props;

	unsigned int do_disconnect:1;
	uint32_t stride;

	char *local_ip;
	int local_port;
	int sess_latency_msec;
};

static void stream_destroy(void *d)
{
	struct impl *data = d;
	spa_hook_remove(&data->playback_listener);
	data->playback = NULL;
}

static void playback_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *dst;

	if ((b = pw_stream_dequeue_buffer(impl->playback)) == NULL) {
		pw_log_debug("Out of playback buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((dst = buf->datas[0].data) == NULL)
		return;

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = impl->stride;
	buf->datas[0].chunk->size = 0;


	buf->datas[0].chunk->size = 0;
	b->size = 0 / impl->stride;

	pw_stream_queue_buffer(impl->playback, b);
}

static void on_core_error(void *d, uint32_t id, int seq, int res, const char *message)
{
	struct impl *data = d;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(data->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *data = d;

	switch (state) {
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("stream disconnected, unloading");
		pw_impl_module_schedule_destroy(data->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_error("stream error: %s", error);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.process = playback_process
};

static void core_destroy(void *d)
{
	struct impl *data = d;
	spa_hook_remove(&data->core_listener);
	data->core = NULL;
	pw_impl_module_schedule_destroy(data->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *data)
{
	if (data->playback)
		pw_stream_destroy(data->playback);
	if (data->core && data->do_disconnect)
		pw_core_disconnect(data->core);

	pw_properties_free(data->playback_props);
	pw_properties_free(data->props);

	free(data->local_ip);
	free(data);
}

static void module_destroy(void *d)
{
	struct impl *data = d;
	spa_hook_remove(&data->module_listener);
	impl_destroy(data);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

struct sdp_info {
	const char *origin;
	const char *session;

	struct sockaddr_storage sa;
	socklen_t salen;

	uint16_t port;
	uint8_t payload;

	struct spa_audio_info_raw info;
};

static int rtp_session_new(struct impl *data, struct sdp_info *sdp)
{
	const struct spa_pod *params[1];
	struct spa_pod_builder b;
	uint32_t n_params;
	uint8_t buffer[1024];
	int res;

	data->stride = sdp->info.channels * sizeof(float);

	pw_properties_setf(data->playback_props, PW_KEY_NODE_RATE, "1/%d", sdp->info.rate);

	data->playback = pw_stream_new(data->core,
			"rtp-source playback", data->playback_props);
	data->playback_props = NULL;
	if (data->playback == NULL)
		return -errno;

	pw_stream_add_listener(data->playback,
			&data->playback_listener,
			&out_stream_events, data);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&sdp->info);

	if ((res = pw_stream_connect(data->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int parse_sdp_c(struct impl *impl, char *c, struct sdp_info *info)
{
	int res;

	c[strcspn(c, "/")] = 0;
	if (spa_strstartswith(c, "c=IN IP4 ")) {
		struct sockaddr_in *sa = (struct sockaddr_in*) &info->sa;

		c += sizeof("c=IN IP4 ");
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

		c += sizeof("c=IN IP6 ");
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

	c += sizeof("m=audio ");
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

static int parse_sdp_a(struct impl *impl, char *c, struct sdp_info *info)
{
	int payload, len, rate, channels;

	if (!spa_strstartswith(c, "a=rtpmap:"))
		return 0;

	c += sizeof("a=rtpmap:");

	if (sscanf(c, "%i %n", &payload, &len) != 1)
		return -EINVAL;

	if (payload < 0 || payload > 127)
		return -EINVAL;

	if (payload != info->payload)
		return 0;

	c += len;
	if (spa_strstartswith(c, "L16/")) {
		info->info.format = SPA_AUDIO_FORMAT_S16_BE;
		c += 4;
	} else
		return -EINVAL;

	if (sscanf(c, "%u/%u", &rate, &channels) == 2) {
		info->info.rate = rate;
		info->info.channels = channels;
		if (channels == 2) {
			info->info.position[0] = SPA_AUDIO_CHANNEL_FL;
			info->info.position[1] = SPA_AUDIO_CHANNEL_FR;
		}
	} else if (sscanf(c, "%u", &rate) == 1) {
		info->info.rate = rate;
		info->info.channels = 1;
	} else
		return -EINVAL;

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
		pw_log_info("%d: %s", count, s);

		if (count++ == 0 && strcmp(s, "v=0") != 0)
			goto invalid_version;

		if (spa_strstartswith(s, "o="))
            		info->origin = &s[2];
		else if (spa_strstartswith(s, "s="))
            		info->session = &s[2];
		else if (spa_strstartswith(s, "c="))
			res = parse_sdp_c(impl, s, info);
		else if (spa_strstartswith(s, "m="))
			res = parse_sdp_m(impl, s, info);
		else if (spa_strstartswith(s, "a="))
			res = parse_sdp_a(impl, s, info);

		if (res < 0)
			goto error;
		s += l + 1;
		while (isspace(*s))
			s++;
        }
	return 0;
too_short:
	pw_log_warn("SDP: line starting with `%.6s...' too short", s);
	return -EINVAL;
invalid_version:
	pw_log_warn("SDP: invalid first version line `%*s'", (int)l, s);
	return -EINVAL;
error:
	return res;
}

static int parse_sap(struct impl *impl, void *data, size_t len)
{
	struct sap_header *header;
	char *mime, *sdp;
	struct sdp_info info;
	int res;

	if (len < 8)
		return -EINVAL;

	header = (struct sap_header*) data;
	if (header->v != 1)
		return -EINVAL;

	mime = SPA_PTROFF(data, 8, char);
	if (spa_strstartswith(mime, "v=0"))
		sdp = mime;
	else if (spa_streq(mime, "application/sdp"))
		sdp = SPA_PTROFF(mime, strlen(mime)+1, char);
	else
		return -EINVAL;

	spa_zero(info);
	if ((res = parse_sdp(impl, sdp, &info)) < 0)
		return res;

	return res;
}

static void
on_sap_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;

	if (mask & SPA_IO_IN) {
		uint8_t buffer[2048];
		ssize_t len;

		pw_log_info("got sap");
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
	int af, fd, val, res;

	if (inet_pton(AF_INET, impl->local_ip, &sa4.sin_addr) > 0) {
		af = sa4.sin_family = AF_INET;
		sa4.sin_port = htons(impl->local_port);
		sa = (struct sockaddr*) &sa4;
		salen = sizeof(sa4);
	} else if (inet_pton(AF_INET6, impl->local_ip, &sa6.sin6_addr) > 0) {
		af = sa6.sin6_family = AF_INET6;
		sa6.sin6_port = htons(impl->local_port);
		sa = (struct sockaddr*) &sa6;
		salen = sizeof(sa6);
	} else
		return -EINVAL;

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
	res = 0;
	if (af == AF_INET) {
		static const uint32_t ipv4_mcast_mask = 0xe0000000;
		if ((ntohl(((const struct sockaddr_in*) sa)->sin_addr.s_addr) &
					ipv4_mcast_mask) == ipv4_mcast_mask) {
			struct ip_mreq mr4;
			memset(&mr4, 0, sizeof(mr4));
			mr4.imr_multiaddr = ((const struct sockaddr_in*) sa)->sin_addr;
			res = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr4, sizeof(mr4));
		}
	} else if (af == AF_INET6) {
		if (((const struct sockaddr_in6*) sa)->sin6_addr.s6_addr[0] == 0xff) {
			struct ipv6_mreq mr6;
			memset(&mr6, 0, sizeof(mr6));
			mr6.ipv6mr_multiaddr = ((const struct sockaddr_in6*) sa)->sin6_addr;
			res = setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mr6, sizeof(mr6));
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
		pw_log_warn("bind() failed: %m");
		goto error;
	}

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

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "rtp source" },
	{ PW_KEY_MODULE_USAGE,	"source.name=<name for the source> "
				"sess.latency.msec=<target network latency in milliseconds> "
				"local.ip=<local receiver ip> "
				"source.props= { key=value ... }" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props = NULL, *playback_props = NULL;
	const char *str;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->props = props;

	playback_props = pw_properties_new(NULL, NULL);
	if (playback_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->playback_props = playback_props;

	impl->module = module;
	impl->module_context = context;
	impl->loop = pw_context_get_main_loop(context);

	if ((str = pw_properties_get(props, "source.name")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source.name", NULL);
	}

	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(playback_props, str, strlen(str));

	if (pw_properties_get(playback_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_NAME, "rtp-source");
	if (pw_properties_get(playback_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_DESCRIPTION, "RTP Source");
	if (pw_properties_get(playback_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(playback_props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_NETWORK, "true");

	if ((str = pw_properties_get(props, "local.ip")) != NULL) {
		impl->local_ip = strdup(str);
		pw_properties_set(props, "local.ip", NULL);
	} else {
		impl->local_ip = strdup(SAP_DEFAULT_IP);
	}
	impl->local_port = SAP_DEFAULT_PORT;

	if ((str = pw_properties_get(props, "sess.latency.msec")) != NULL) {
		impl->sess_latency_msec = pw_properties_parse_int(str);
		pw_properties_set(props, "sess.latency.msec", NULL);
	} else {
		impl->sess_latency_msec = DEFAULT_SESS_LATENCY;
	}

	impl->core = pw_context_get_object(impl->module_context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
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
