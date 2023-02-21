/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
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
#include <spa/utils/dll.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format-utils.h>
#include <spa/control/control.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/rtp.h>

#ifdef __FreeBSD__
#define ifr_ifindex ifr_index
#endif

/** \page page_module_rtp_source PipeWire Module: RTP source
 *
 * The `rtp-source` module creates a PipeWire source that receives audio
 * and midi RTP packets.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `local.ifname = <str>`: interface name to use
 * - `node.always-process = <bool>`: true to receive even when not running
 * - `sess.latency.msec = <str>`: target network latency in milliseconds, default 100
 * - `rtp.media = <string>`: the media type audio|midi, default audio
 * - `stream.props = {}`: properties to be passed to the stream
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
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
 * context.modules = [
 * {   name = libpipewire-module-rtp-source
 *     args = {
 *         #local.ifname = eth0
 *         sess.latency.msec = 100
 *         #node.always-process = false
 *         #rtp.media = "audio"
 *         #audio.format = "S16BE"
 *         #audio.rate = 48000
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *            #media.class = "Audio/Source"
 *            node.name = "rtp-source"
 *         }
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

#define DEFAULT_CLEANUP_SEC		60
#define ERROR_MSEC			2

#define DEFAULT_SESS_LATENCY		100

#define DEFAULT_SOURCE_IP		"224.0.0.56"

#define DEFAULT_FORMAT			"S16BE"
#define DEFAULT_RATE			48000
#define DEFAULT_CHANNELS		2
#define DEFAULT_POSITION		"[ FL FR ]"

#define BUFFER_SIZE			(1u<<22)
#define BUFFER_MASK			(BUFFER_SIZE-1)
#define BUFFER_SIZE2			(BUFFER_SIZE>>1)
#define BUFFER_MASK2			(BUFFER_SIZE2-1)

#define USAGE   "local.ifname=<local interface name to use> "						\
		"source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> "				\
 		"source.port=<int, source port> "							\
		"sess.latency.msec=<target network latency, default "SPA_STRINGIFY(DEFAULT_SESS_LATENCY)"> "	\
 		"rtp.media=<string, the media type audio|midi, default audio> "		\
		"audio.format=<format, default:"DEFAULT_FORMAT"> "				\
		"audio.rate=<sample rate, default:"SPA_STRINGIFY(DEFAULT_RATE)"> "		\
		"audio.channels=<number of channels, default:"SPA_STRINGIFY(DEFAULT_CHANNELS)"> "\
		"audio.position=<channel map, default:"DEFAULT_POSITION"> "			\
		"stream.props= { key=value ... } "

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
	unsigned int do_disconnect:1;

	char *ifname;
	bool always_process;
	int sess_latency_msec;
	uint32_t cleanup_interval;

	struct spa_source *timer;

	struct spa_audio_info info;
	struct pw_properties *stream_props;
	struct pw_stream *stream;
	struct spa_hook stream_listener;

	uint16_t src_port;
	struct sockaddr_storage src_addr;
	socklen_t src_len;
	struct spa_source *source;

	uint32_t rate;
	uint32_t stride;
	uint32_t expected_ssrc;
	uint16_t expected_seq;
	unsigned have_ssrc:1;
	unsigned have_seq:1;
	unsigned have_sync:1;
	uint32_t ts_offset;

	struct spa_ringbuffer ring;
	uint8_t buffer[BUFFER_SIZE];

	struct spa_io_rate_match *rate_match;
	struct spa_io_position *position;
	struct spa_dll dll;
	double corr;
	uint32_t target_buffer;
	float max_error;

	unsigned first:1;
	unsigned receiving:1;
	unsigned direct_timestamp:1;

	float last_timestamp;
	float last_time;
};

struct format_info {
	uint32_t media_subtype;
	uint32_t format;
	uint32_t size;
	const char *mime;
	const char *media_type;
};

static uint32_t audio_get_stride(const struct spa_audio_info *info)
{
	uint32_t stride = 0;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return 0;

	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_ALAW:
	case SPA_AUDIO_FORMAT_ULAW:
		stride = 1;
		break;
	case SPA_AUDIO_FORMAT_S16_BE:
		stride = 2;
		break;
	case SPA_AUDIO_FORMAT_S24_BE:
		stride = 3;
		break;
	default:
		break;
	}
	return stride * info->info.raw.channels;
}

static void process_audio(struct impl *impl)
{
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t wanted, timestamp, target_buffer, stride, maxsize;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	stride = impl->stride;

	maxsize = d[0].maxsize / stride;
	wanted = buf->requested ? SPA_MIN(buf->requested, maxsize) : maxsize;

	if (impl->position && impl->direct_timestamp) {
		/* in direct mode, read directly from the timestamp index,
		 * because sender and receiver are in sync, this would keep
		 * target_buffer of samples available. */
		spa_ringbuffer_read_update(&impl->ring,
				impl->position->clock.position);
	}
	avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);

	target_buffer = impl->target_buffer;

	if (avail < (int32_t)wanted) {
		enum spa_log_level level;
		memset(d[0].data, 0, wanted * stride);
		if (impl->have_sync) {
			impl->have_sync = false;
			level = SPA_LOG_LEVEL_WARN;
		} else {
			level = SPA_LOG_LEVEL_DEBUG;
		}
		pw_log(level, "underrun %d/%u < %u",
					avail, target_buffer, wanted);
	} else {
		float error, corr;
		if (impl->first) {
			if ((uint32_t)avail > target_buffer) {
				uint32_t skip = avail - target_buffer;
				pw_log_debug("first: avail:%d skip:%u target:%u",
							avail, skip, target_buffer);
				timestamp += skip;
				avail = target_buffer;
			}
			impl->first = false;
		} else if (avail > (int32_t)SPA_MIN(target_buffer * 8, BUFFER_SIZE / stride)) {
			pw_log_warn("overrun %u > %u", avail, target_buffer * 8);
			timestamp += avail - target_buffer;
			avail = target_buffer;
		}
		if (!impl->direct_timestamp) {
			/* when not using direct timestamp and clocks are not
			 * in sync, try to adjust our playback rate to keep the
			 * requested target_buffer bytes in the ringbuffer */
			error = (float)target_buffer - (float)avail;
			error = SPA_CLAMP(error, -impl->max_error, impl->max_error);

			corr = spa_dll_update(&impl->dll, error);

			pw_log_debug("avail:%u target:%u error:%f corr:%f", avail,
					target_buffer, error, corr);

			if (impl->rate_match) {
				SPA_FLAG_SET(impl->rate_match->flags,
						SPA_IO_RATE_MATCH_FLAG_ACTIVE);
				impl->rate_match->rate = 1.0f / corr;
			}
		}
		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(timestamp * stride) & BUFFER_MASK,
				d[0].data, wanted * stride);

		timestamp += wanted;
		spa_ringbuffer_read_update(&impl->ring, timestamp);
	}
	d[0].chunk->size = wanted * stride;
	d[0].chunk->stride = stride;
	d[0].chunk->offset = 0;
	buf->size = wanted;

	pw_stream_queue_buffer(impl->stream, buf);
}

static void receive_audio(struct impl *impl, uint8_t *packet,
		uint32_t timestamp, uint32_t payload_offset, uint32_t len)
{
	uint32_t plen = len - payload_offset;
	uint8_t *payload = &packet[payload_offset];
	uint32_t stride = impl->stride;
	uint32_t samples = plen / stride;
	uint32_t write, expected_write;
	int32_t filled;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_write);

	/* we always write to timestamp + delay */
	write = timestamp + impl->target_buffer;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp %u direct:%d ts_offset:%u",
				write, impl->direct_timestamp, impl->ts_offset);
		/* we read from timestamp, keeping target_buffer of data
		 * in the ringbuffer. */
		impl->ring.readindex = timestamp;
		impl->ring.writeindex = write;
		filled = impl->target_buffer;

		spa_dll_init(&impl->dll);
		spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);
		memset(impl->buffer, 0, BUFFER_SIZE);
		impl->have_sync = true;
	} else if (expected_write != write) {
		pw_log_debug("unexpected write (%u != %u)",
				write, expected_write);
	}

	if (filled + samples > BUFFER_SIZE / stride) {
		pw_log_debug("capture overrun %u + %u > %u", filled, samples,
				BUFFER_SIZE / stride);
		impl->have_sync = false;
	} else {
		pw_log_debug("got samples:%u", samples);
		spa_ringbuffer_write_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(write * stride) & BUFFER_MASK,
				payload, (samples * stride));
		write += samples;
		spa_ringbuffer_write_update(&impl->ring, write);
	}
}

static void process_midi(struct impl *impl)
{
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t timestamp, duration, maxsize, read;
	struct spa_pod_builder b;
	struct spa_pod_frame f[1];
	void *ptr;
	struct spa_pod *pod;
	struct spa_pod_control *c;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	maxsize = d[0].maxsize;

	/* we always use the graph position to select events, the receiver side is
	 * responsible for smoothing out the RTP timestamps to graph time */
	duration = impl->position->clock.duration;
	if (impl->position)
		timestamp = impl->position->clock.position;
	else
		timestamp = 0;

	/* we copy events into the buffer based on the rtp timestamp + delay. */
	spa_pod_builder_init(&b, d[0].data, maxsize);
	spa_pod_builder_push_sequence(&b, &f[0], 0);

	while (true) {
		int32_t avail = spa_ringbuffer_get_read_index(&impl->ring, &read);
		if (avail <= 0)
			break;

		ptr = SPA_PTROFF(impl->buffer, read & BUFFER_MASK2, void);

		if ((pod = spa_pod_from_data(ptr, avail, 0, avail)) == NULL)
			goto done;
		if (!spa_pod_is_sequence(pod))
			goto done;

		/* the ringbuffer contains series of sequences, one for each
		 * received packet */
		SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence*)pod, c) {
			/* try to render with given delay */
			uint32_t target = c->offset + impl->target_buffer;
			if (timestamp != 0) {
				/* skip old packets */
				if (target < timestamp)
					continue;
				/* event for next cycle */
				if (target >= timestamp + duration)
					goto complete;
			} else {
				timestamp = target;
			}
			spa_pod_builder_control(&b, target - timestamp, SPA_CONTROL_Midi);
			spa_pod_builder_bytes(&b,
					SPA_POD_BODY(&c->value),
					SPA_POD_BODY_SIZE(&c->value));
		}
		/* we completed a sequence (one RTP packet), advance ringbuffer
		 * and go to the next packet */
		read += SPA_PTRDIFF(c, ptr);
		spa_ringbuffer_read_update(&impl->ring, read);
	}
complete:
	spa_pod_builder_pop(&b, &f[0]);

	if (b.state.offset > maxsize) {
		pw_log_warn("overflow buffer %u %u", b.state.offset, maxsize);
		b.state.offset = 0;
	}
	d[0].chunk->size = b.state.offset;
	d[0].chunk->stride = 1;
	d[0].chunk->offset = 0;
done:
	pw_stream_queue_buffer(impl->stream, buf);
}

static int parse_varlen(uint8_t *p, uint32_t avail, uint32_t *result)
{
	uint32_t value = 0, offs = 0;
	while (offs < avail) {
		uint8_t b = p[offs++];
		value = (value << 7) | (b & 0x7f);
		if ((b & 0x80) == 0)
			break;
	}
	*result = value;
	return offs;
}

static int get_midi_size(uint8_t *p, uint32_t avail)
{
	int size;
	uint32_t offs = 0, value;

	switch (p[offs++]) {
	case 0xc0 ... 0xdf:
		size = 2;
		break;
	case 0x80 ... 0xbf:
	case 0xe0 ... 0xef:
		size = 3;
		break;
	case 0xff:
	case 0xf0:
	case 0xf7:
		size = parse_varlen(&p[offs], avail - offs, &value);
		size += value + 1;
		break;
	default:
		return -EINVAL;
	}
	return size;
}

static double get_time(struct impl *impl)
{
	struct timespec ts;
	double t;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t = impl->position->clock.position / (double) impl->position->clock.rate.denom;
	t += (SPA_TIMESPEC_TO_NSEC(&ts) - impl->position->clock.nsec) / (double)SPA_NSEC_PER_SEC;
	return t;
}

static void receive_midi(struct impl *impl, uint8_t *packet,
		uint32_t timestamp, uint32_t payload_offset, uint32_t plen)
{
	uint32_t write;
	struct rtp_midi_header *hdr;
	int32_t filled;
	struct spa_pod_builder b;
	struct spa_pod_frame f[1];
	void *ptr;
	uint32_t offs = payload_offset, len, end;
	bool first = true;

	if (impl->direct_timestamp) {
		/* in direct timestamp we attach the RTP timestamp directly on the
		 * midi events and render them in the corresponding cycle */
		if (!impl->have_sync) {
			pw_log_info("sync to timestamp %u/ direct:%d", timestamp,
					impl->direct_timestamp);
			impl->have_sync = true;
		}
	} else {
		/* in non-direct timestamp mode, we relate the graph clock against
		 * the RTP timestamps */
		double ts = timestamp / (float) impl->rate;
		double t = get_time(impl);
		double elapsed, estimated, diff;

		/* the elapsed time between RTP timestamps */
		elapsed = ts - impl->last_timestamp;
		/* for that elapsed time, our clock should have advanced
		 * by this amount since the last estimation */
		estimated = impl->last_time + elapsed * impl->corr;
		/* calculate the diff between estimated and current clock time in
		 * samples */
		diff = (estimated - t) * impl->rate;

		/* no sync or we drifted too far, resync */
		if (!impl->have_sync || fabs(diff) > impl->target_buffer) {
			impl->corr = 1.0;
			spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 256, impl->rate);

			pw_log_info("sync to timestamp %u/%f direct:%d", timestamp, t,
					impl->direct_timestamp);
			impl->have_sync = true;
			impl->ring.readindex = impl->ring.writeindex;
		} else {
			/* update our new rate correction */
			impl->corr = spa_dll_update(&impl->dll, diff);
			/* our current time is now the estimated time */
			t = estimated;
		}
		pw_log_debug("%f %f %f %f", t, estimated, diff, impl->corr);

		timestamp = t * impl->rate;

		impl->last_timestamp = ts;
		impl->last_time = t;
	}

	filled = spa_ringbuffer_get_write_index(&impl->ring, &write);
	if (filled > (int32_t)BUFFER_SIZE2)
		return;

	hdr = (struct rtp_midi_header *)&packet[offs++];
	len = hdr->len;
	if (hdr->b) {
		len = (len << 8) | hdr->len_b;
		offs++;
	}
	end = len + offs;
	if (end > plen)
		return;

	ptr = SPA_PTROFF(impl->buffer, write & BUFFER_MASK2, void);

	/* each packet is written as a sequence of events. The offset is
	 * the RTP timestamp */
	spa_pod_builder_init(&b, ptr, BUFFER_SIZE2 - filled);
	spa_pod_builder_push_sequence(&b, &f[0], 0);

	while (offs < end) {
		uint32_t delta;
		int size;

		if (first && !hdr->z)
			delta = 0;
		else
			offs += parse_varlen(&packet[offs], end - offs, &delta);

		timestamp += delta * impl->corr;
		spa_pod_builder_control(&b, timestamp, SPA_CONTROL_Midi);

		size = get_midi_size(&packet[offs], end - offs);

		if (size <= 0 || offs + size > end) {
			pw_log_warn("invalid size (%08x) %d (%u %u)",
					packet[offs], size, offs, end);
			break;
		}

		spa_pod_builder_bytes(&b, &packet[offs], size);

		offs += size;
		first = false;
	}
	spa_pod_builder_pop(&b, &f[0]);

	write += b.state.offset;
	spa_ringbuffer_write_update(&impl->ring, write);
}

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_IO_RateMatch:
		impl->rate_match = area;
		break;
	case SPA_IO_Position:
		impl->position = area;
		break;
	}
}

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static void stream_process(void *data)
{
	struct impl *impl = data;
	switch (impl->info.media_type) {
	case SPA_MEDIA_TYPE_audio:
		process_audio(impl);
		break;
	case SPA_MEDIA_TYPE_application:
		process_midi(impl);
		break;
	}
}

static void
on_rtp_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	struct rtp_header *hdr;
	ssize_t len, hlen;
	uint8_t buffer[2048];

	if (mask & SPA_IO_IN) {
		uint16_t seq;
		uint32_t timestamp;

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

		if (impl->have_ssrc && impl->expected_ssrc != hdr->ssrc)
			goto unexpected_ssrc;
		impl->expected_ssrc = hdr->ssrc;
		impl->have_ssrc = true;

		seq = ntohs(hdr->sequence_number);
		if (impl->have_seq && impl->expected_seq != seq) {
			pw_log_info("unexpected seq (%d != %d)", seq, impl->expected_seq);
			impl->have_sync = false;
		}
		impl->expected_seq = seq + 1;
		impl->have_seq = true;

		timestamp = ntohl(hdr->timestamp) - impl->ts_offset;

		switch (impl->info.media_type) {
		case SPA_MEDIA_TYPE_audio:
			receive_audio(impl, buffer, timestamp, hlen, len);
			break;
		case SPA_MEDIA_TYPE_application:
			receive_midi(impl, buffer, timestamp, hlen, len);
		}
		impl->receiving = true;
	}
	return;

receive_error:
	pw_log_warn("recv error: %m");
	return;
short_packet:
	pw_log_warn("short packet received");
	return;
invalid_version:
	pw_log_warn("invalid RTP version");
	spa_debug_mem(0, buffer, len);
	return;
invalid_len:
	pw_log_warn("invalid RTP length");
	return;
unexpected_ssrc:
	pw_log_warn("unexpected SSRC (expected %u != %u)",
		impl->expected_ssrc, hdr->ssrc);
	return;
}

static int parse_address(const char *address, uint16_t port,
		struct sockaddr_storage *addr, socklen_t *len)
{
	struct sockaddr_in *sa4 = (struct sockaddr_in*)addr;
	struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)addr;

	if (inet_pton(AF_INET, address, &sa4->sin_addr) > 0) {
		sa4->sin_family = AF_INET;
		sa4->sin_port = htons(port);
		*len = sizeof(*sa4);
	} else if (inet_pton(AF_INET6, address, &sa6->sin6_addr) > 0) {
		sa6->sin6_family = AF_INET6;
		sa6->sin6_port = htons(port);
		*len = sizeof(*sa6);
	} else
		return -EINVAL;

	return 0;
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

static uint32_t msec_to_samples(struct impl *impl, uint32_t msec)
{
	return msec * impl->rate / 1000;
}

static int stream_start(struct impl *impl)
{
	int fd;

	if (impl->source != NULL)
		return 0;

	pw_log_info("starting RTP listener");

	if ((fd = make_socket((const struct sockaddr *)&impl->src_addr,
					impl->src_len, impl->ifname)) < 0) {
		pw_log_error("failed to create socket: %m");
		return fd;
	}

	impl->source = pw_loop_add_io(impl->data_loop, fd,
				SPA_IO_IN, true, on_rtp_io, impl);
	if (impl->source == NULL) {
		pw_log_error("can't create io source: %m");
		close(fd);
		return -errno;
	}
	return 0;
}

static void stream_stop(struct impl *impl)
{
	if (!impl->source)
		return;

	pw_log_info("stopping RTP listener");

	pw_loop_destroy_source(impl->data_loop, impl->source);
	impl->source = NULL;
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;

	switch (state) {
		case PW_STREAM_STATE_UNCONNECTED:
			pw_log_info("stream disconnected, unloading");
			pw_impl_module_schedule_destroy(impl->module);
			break;
		case PW_STREAM_STATE_ERROR:
			pw_log_error("stream error: %s", error);
			break;
		case PW_STREAM_STATE_STREAMING:
			if ((errno = -stream_start(impl)) < 0)
				pw_log_error("failed to start RTP stream: %m");
			break;
		case PW_STREAM_STATE_PAUSED:
			if (!impl->always_process)
				stream_stop(impl);
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

static int setup_stream(struct impl *impl)
{
	const struct spa_pod *params[1];
	struct spa_pod_builder b;
	uint32_t n_params;
	enum pw_stream_flags flags;
	uint8_t buffer[1024];
	struct pw_properties *props;
	int res;

	impl->first = true;

	props = pw_properties_copy(impl->stream_props);
	if (props == NULL) {
		res = -errno;
		goto error;
	}

	spa_dll_init(&impl->dll);
	spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);
	impl->corr = 1.0;

	impl->stream = pw_stream_new(impl->core,
			"rtp-source playback", props);
	if (impl->stream == NULL) {
		res = -errno;
		pw_log_error("can't create stream: %m");
		goto error;
	}

	pw_stream_add_listener(impl->stream,
			&impl->stream_listener,
			&out_stream_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;

	switch (impl->info.media_type) {
	case SPA_MEDIA_TYPE_audio:
		params[n_params++] = spa_format_audio_build(&b,
				SPA_PARAM_EnumFormat, &impl->info);
		flags |= PW_STREAM_FLAG_AUTOCONNECT;
		break;
	case SPA_MEDIA_TYPE_application:
		params[n_params++] = spa_pod_builder_add_object(&b,
                                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                                SPA_FORMAT_mediaType,           SPA_POD_Id(SPA_MEDIA_TYPE_application),
                                SPA_FORMAT_mediaSubtype,        SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;
	default:
		return -EINVAL;
	}

	if ((res = pw_stream_connect(impl->stream,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			flags,
			params, n_params)) < 0) {
		pw_log_error("can't connect stream: %s", spa_strerror(res));
		goto error;
	}

	if (impl->always_process &&
		(res = stream_start(impl)) < 0)
		goto error;

	return 0;
error:
	return res;
}

static void on_timer_event(void *data, uint64_t expirations)
{
	struct impl *impl = data;

	if (!impl->receiving) {
		pw_log_info("timeout, inactive RTP source");
		//pw_impl_module_schedule_destroy(impl->module);
	} else {
		pw_log_debug("timeout, keeping active RTP source");
	}
	impl->receiving = false;
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
	if (impl->stream)
		pw_stream_destroy(impl->stream);
	if (impl->source)
		pw_loop_destroy_source(impl->data_loop, impl->source);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->timer)
		pw_loop_destroy_source(impl->loop, impl->timer);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->ifname);
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

static inline uint32_t format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) == NULL)
		str = DEFAULT_FORMAT;
	info->format = format_from_name(str, strlen(str));

	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
	if (info->rate == 0)
		info->rate = DEFAULT_RATE;

	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

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
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t pid = getpid();
	struct impl *impl;
	const char *str;
	struct timespec value, interval;
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
	impl->module_context = context;
	impl->loop = pw_context_get_main_loop(context);
	impl->data_loop = pw_data_loop_get_loop(pw_context_get_data_loop(context));

	if (pw_properties_get(stream_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(stream_props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(stream_props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(stream_props, PW_KEY_NODE_NETWORK, "true");

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "rtp-source-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION,
				pw_properties_get(props, PW_KEY_NODE_NAME));
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_NAME, "RTP Receiver Stream");

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_CHANNELNAMES);
	copy_props(impl, props, PW_KEY_MEDIA_NAME);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);

	impl->info.media_type = SPA_MEDIA_TYPE_audio;
	impl->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	if ((str = pw_properties_get(stream_props, "rtp.media")) != NULL) {
		if (spa_streq(str, "audio")) {
			impl->info.media_type = SPA_MEDIA_TYPE_audio;
			impl->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		}
		else if (spa_streq(str, "midi")) {
			impl->info.media_type = SPA_MEDIA_TYPE_application;
			impl->info.media_subtype = SPA_MEDIA_SUBTYPE_control;
		}
		else {
			pw_log_error("unsupported media type:%s", str);
			res = -EINVAL;
			goto out;
		}
	}

	switch (impl->info.media_type) {
	case SPA_MEDIA_TYPE_audio:
		parse_audio_info(stream_props, &impl->info.info.raw);
		impl->stride = audio_get_stride(&impl->info);
		if (impl->stride == 0) {
			pw_log_error("unsupported audio format:%d channels:%d",
					impl->info.info.raw.format, impl->info.info.raw.channels);
			res = -EINVAL;
			goto out;
		}
		impl->rate = impl->info.info.raw.rate;
		break;
	case SPA_MEDIA_TYPE_application:
		pw_properties_set(stream_props, PW_KEY_FORMAT_DSP, "8 bit raw midi");
		impl->stride = 1;
		impl->rate = 48000;
		break;
	default:
		spa_assert_not_reached();
		break;
	}

	str = pw_properties_get(props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	impl->src_port = pw_properties_get_uint32(stream_props, "source.port", 0);
	if (impl->src_port == 0) {
		pw_log_error("invalid source.port");
		goto out;
	}
	if ((str = pw_properties_get(stream_props, "source.ip")) == NULL ||
	    (res = parse_address(str, impl->src_port, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	impl->always_process = pw_properties_get_bool(stream_props,
			PW_KEY_NODE_ALWAYS_PROCESS, true);

	impl->cleanup_interval = pw_properties_get_uint32(props,
			"cleanup.sec", DEFAULT_CLEANUP_SEC);

	if ((str = pw_properties_get(props, "sess.name")) != NULL) {
		pw_properties_set(stream_props, "rtp.session", str);
		if (pw_properties_get(stream_props, PW_KEY_MEDIA_NAME) == NULL)
			pw_properties_setf(stream_props, PW_KEY_MEDIA_NAME, "RTP Stream (%s)", str);
		if (pw_properties_get(stream_props, PW_KEY_NODE_NAME) == NULL)
			pw_properties_setf(stream_props, PW_KEY_NODE_NAME, "%s", str);
	} else {
		if (pw_properties_get(stream_props, PW_KEY_MEDIA_NAME) == NULL)
			pw_properties_set(stream_props, PW_KEY_MEDIA_NAME, "RTP Stream");
	}

	impl->ts_offset = pw_properties_get_int64(stream_props, "sess.ts-offset", 0);
	pw_properties_setf(stream_props, "rtp.ts-offset", "%u", impl->ts_offset);

	impl->direct_timestamp = pw_properties_get_bool(stream_props,
			"sess.ts-direct", false);

	impl->sess_latency_msec = pw_properties_get_uint32(stream_props,
			"sess.latency.msec", DEFAULT_SESS_LATENCY);
	impl->target_buffer = msec_to_samples(impl, impl->sess_latency_msec);
	impl->max_error = msec_to_samples(impl, ERROR_MSEC);

	pw_properties_setf(stream_props, PW_KEY_NODE_RATE, "1/%d", impl->rate);
	pw_properties_setf(stream_props, PW_KEY_NODE_LATENCY, "%d/%d",
			impl->target_buffer / 2, impl->rate);

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

	impl->timer = pw_loop_add_timer(impl->loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto out;
	}
	value.tv_sec = impl->cleanup_interval;
	value.tv_nsec = 0;
	interval.tv_sec = impl->cleanup_interval;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->loop, impl->timer, &value, &interval, false);

	if ((res = setup_stream(impl)) < 0)
		goto out;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-rtp-source");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
