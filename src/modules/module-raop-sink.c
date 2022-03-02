/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>
#include <openssl/aes.h>
#include <openssl/md5.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/debug/pod.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/latency-utils.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>
#include <pipewire/private.h>

#include "module-raop/rtsp-client.h"

/** \page page_module_raop_sink PipeWire Module: AirPlay Sink
 */

#define NAME "raop-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define FRAMES_PER_TCP_PACKET 4096
#define FRAMES_PER_UDP_PACKET 352

#define DEFAULT_TCP_AUDIO_PORT   6000
#define DEFAULT_UDP_AUDIO_PORT   6000
#define DEFAULT_UDP_CONTROL_PORT 6001
#define DEFAULT_UDP_TIMING_PORT  6002

#define AES_CHUNK_SIZE		16
#ifndef MD5_DIGEST_LENGTH
#define MD5_DIGEST_LENGTH	16
#endif
#define MD5_HASH_LENGTH (2*MD5_DIGEST_LENGTH)

#define DEFAULT_USER_AGENT	"iTunes/11.0.4 (Windows; N)"
#define DEFAULT_USER_NAME	"iTunes"

#define MAX_PORT_RETRY	128

#define DEFAULT_FORMAT "S16"
#define DEFAULT_RATE 44100
#define DEFAULT_CHANNELS "2"
#define DEFAULT_POSITION "[ FL FR ]"

#define DEFAULT_LATENCY (DEFAULT_RATE*2)

#define MODULE_USAGE	"[ node.latency=<latency as fraction> ] "				\
			"[ node.name=<name of the nodes> ] "					\
			"[ node.description=<description of the nodes> ] "			\
			"[ audio.format=<format, default:"DEFAULT_FORMAT"> ] "			\
			"[ audio.rate=<sample rate, default: 48000> ] "				\
			"[ audio.channels=<number of channels, default:"DEFAULT_CHANNELS"> ] "	\
			"[ audio.position=<channel map, default:"DEFAULT_POSITION"> ] "		\
			"[ stream.props=<properties> ] "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "An RAOP audio sink" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

enum {
	PROTO_TCP,
	PROTO_UDP,
};
enum {
	CRYPTO_NONE,
	CRYPTO_RSA,
};
enum {
	CODEC_PCM,
	CODEC_ALAC,
	CODEC_AAC,
	CODEC_AAC_ELD,
};

struct impl {
	struct pw_context *context;

	struct pw_properties *props;

	struct pw_impl_module *module;
	struct pw_loop *loop;

	struct spa_hook module_listener;

	int protocol;
	int encryption;
	int codec;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_properties *stream_props;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_audio_info_raw info;
	uint32_t frame_size;

	struct pw_rtsp_client *rtsp;
	struct spa_hook rtsp_listener;
	struct pw_properties *headers;

	char session_id[32];
	char *password;

	unsigned int do_disconnect:1;

	uint8_t key[AES_CHUNK_SIZE]; /* Key for aes-cbc */
	uint8_t iv[AES_CHUNK_SIZE];  /* Initialization vector for cbc */
	AES_KEY aes;                 /* AES encryption */

	uint16_t control_port;
	int control_fd;
	struct spa_source *control_source;

	uint16_t timing_port;
	int timing_fd;
	struct spa_source *timing_source;

	uint16_t server_port;
	int server_fd;
	struct spa_source *server_source;

	uint32_t block_size;
	uint32_t delay;
	uint32_t latency;

	uint16_t seq;
	uint32_t rtptime;
	uint32_t ssrc;
	uint32_t sync;
	uint32_t sync_period;
	unsigned int first:1;
	unsigned int connected:1;
	unsigned int ready:1;
	unsigned int recording:1;

	uint8_t buffer[FRAMES_PER_TCP_PACKET * 4];
	uint32_t filled;
};

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static inline void bit_writer(uint8_t **p, int *pos, uint8_t data, int len)
{
	int rb = 8 - *pos - len;
	if (rb >= 0) {
		**p = (*pos ? **p : 0) | (data << rb);
                *pos += len;
	} else {
		*(*p)++ |= (data >> -rb);
		**p = data << (8+rb);
		*pos = -rb;
	}
}

static int aes_encrypt(struct impl *impl, uint8_t *data, int len)
{
    uint8_t nv[AES_CHUNK_SIZE];
    uint8_t *buffer;
    int i, j;

    memcpy(nv, impl->iv, AES_CHUNK_SIZE);
    for (i = 0; i + AES_CHUNK_SIZE <= len; i += AES_CHUNK_SIZE) {
        buffer = data + i;
        for (j = 0; j < AES_CHUNK_SIZE; j++)
            buffer[j] ^= nv[j];

        AES_encrypt(buffer, buffer, &impl->aes);

        memcpy(nv, buffer, AES_CHUNK_SIZE);
    }
    return i;
}

static inline uint64_t timespec_to_ntp(struct timespec *ts)
{
    uint64_t ntp = (uint64_t) ts->tv_nsec * UINT32_MAX / SPA_NSEC_PER_SEC;
    return ntp | (uint64_t) (ts->tv_sec + 0x83aa7e80) << 32;
}

static inline uint64_t ntp_now(int clockid)
{
	struct timespec now;
	clock_gettime(clockid, &now);
	return timespec_to_ntp(&now);
}

static int send_udp_sync_packet(struct impl *impl)
{
	uint32_t pkt[5];
	uint32_t rtptime = impl->rtptime;
	uint32_t delay = impl->delay;
	uint64_t transmitted;

	pkt[0] = htonl(0x80d40007);
	if (impl->first)
		pkt[0] |= htonl(0x10000000);
	rtptime -= delay;
	pkt[1] = htonl(rtptime);
	transmitted = ntp_now(CLOCK_MONOTONIC);
	pkt[2] = htonl(transmitted >> 32);
	pkt[3] = htonl(transmitted & 0xffffffff);
	rtptime += delay;
	pkt[4] = htonl(rtptime);

	pw_log_debug("sync: delayed:%u now:%"PRIu64" rtptime:%u",
			rtptime - delay, transmitted, rtptime);

	return write(impl->control_fd, pkt, sizeof(pkt));
}

static int send_udp_timing_packet(struct impl *impl, uint64_t remote, uint64_t received)
{
	uint32_t pkt[8];
	uint64_t transmitted;

	pkt[0] = htonl(0x80d30007);
	pkt[1] = 0x00000000;
	pkt[2] = htonl(remote >> 32);
	pkt[3] = htonl(remote & 0xffffffff);
	pkt[4] = htonl(received >> 32);
	pkt[5] = htonl(received & 0xffffffff);
	transmitted = ntp_now(CLOCK_MONOTONIC);
	pkt[6] = htonl(transmitted >> 32);
	pkt[7] = htonl(transmitted & 0xffffffff);

	pw_log_debug("sync: remote:%"PRIu64" received:%"PRIu64" transmitted:%"PRIu64,
			remote, received, transmitted);

	return write(impl->timing_fd, pkt, sizeof(pkt));
}

static int write_codec_pcm(void *dst, void *frames, uint32_t n_frames)
{
	uint8_t *bp, *b, *d = frames;
	int bpos = 0;
	uint32_t i;

	b = bp = dst;

	bit_writer(&bp, &bpos, 1, 3); /* channel=1, stereo */
	bit_writer(&bp, &bpos, 0, 4); /* Unknown */
	bit_writer(&bp, &bpos, 0, 8); /* Unknown */
	bit_writer(&bp, &bpos, 0, 4); /* Unknown */
	bit_writer(&bp, &bpos, 1, 1); /* Hassize */
	bit_writer(&bp, &bpos, 0, 2); /* Unused */
	bit_writer(&bp, &bpos, 1, 1); /* Is-not-compressed */
	bit_writer(&bp, &bpos, (n_frames >> 24) & 0xff, 8);
	bit_writer(&bp, &bpos, (n_frames >> 16) & 0xff, 8);
	bit_writer(&bp, &bpos, (n_frames >> 8)  & 0xff, 8);
	bit_writer(&bp, &bpos, (n_frames)       & 0xff, 8);

	for (i = 0; i < n_frames; i++) {
		bit_writer(&bp, &bpos, *(d + 1), 8);
		bit_writer(&bp, &bpos, *(d + 0), 8);
		bit_writer(&bp, &bpos, *(d + 3), 8);
		bit_writer(&bp, &bpos, *(d + 2), 8);
		d += 4;
	}
	return bp - b + 1;
}

static int flush_to_udp_packet(struct impl *impl)
{
	const size_t max = 12 + 8 + impl->block_size;
	uint32_t pkt[max], len, n_frames;
	uint8_t *dst;
	int res;

	if (!impl->recording)
		return 0;

	impl->sync++;
	if (impl->first || impl->sync == impl->sync_period) {
		impl->sync = 0;
		send_udp_sync_packet(impl);
	}
	pkt[0] = htonl(0x80600000);
	if (impl->first)
		pkt[0] |= htonl((uint32_t)0x80 << 16);
	pkt[0] |= htonl((uint32_t)impl->seq);
	pkt[1] = htonl(impl->rtptime);
	pkt[2] = htonl(impl->ssrc);

	n_frames = impl->filled / impl->frame_size;
	dst = (uint8_t*)&pkt[3];

	switch (impl->codec) {
	case CODEC_PCM:
		len = write_codec_pcm(dst, impl->buffer, n_frames);
		break;
	default:
		len = 8 + impl->block_size;
		memset(dst, 0, len);
		break;
	}
	if (impl->encryption != CRYPTO_NONE)
		aes_encrypt(impl, dst, len);

	impl->rtptime += n_frames;
	impl->seq = (impl->seq + 1) & 0xffff;

	pw_log_debug("send %u", len + 12);
	res = write(impl->server_fd, pkt, len + 12);

	impl->first = false;

	return res;
}

static int flush_to_tcp_packet(struct impl *impl)
{
	const size_t max = 16 + 8 + impl->block_size;
	uint32_t pkt[max], len, n_frames;
	uint8_t *dst;
	int res;

	if (!impl->recording)
		return 0;

	pkt[0] = htonl(0x24000000);
	pkt[1] = htonl(0x80e00000);
	pkt[1] |= htonl((uint32_t)impl->seq);
	pkt[2] = htonl(impl->rtptime);
	pkt[3] = htonl(impl->ssrc);

	n_frames = impl->filled / impl->frame_size;
	dst = (uint8_t*)&pkt[4];

	switch (impl->codec) {
	case CODEC_PCM:
		len = write_codec_pcm(dst, impl->buffer, n_frames);
		break;
	default:
		len = 8 + impl->block_size;
		memset(dst, 0, len);
		break;
	}
	if (impl->encryption != CRYPTO_NONE)
		aes_encrypt(impl, dst, len);

	pkt[0] |= htonl((uint32_t) len + 12);

	impl->rtptime += n_frames;
	impl->seq = (impl->seq + 1) & 0xffff;

	pw_log_debug("send %u", len + 16);
	res = write(impl->server_fd, pkt, len + 16);

	impl->first = false;

	return res;
}

static void playback_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	uint8_t *data;
	uint32_t size;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	bd = &buf->buffer->datas[0];
	data = SPA_PTROFF(bd->data, bd->chunk->offset, uint8_t);
	size = bd->chunk->size;

	while (size > 0 && impl->block_size > 0) {
		uint32_t avail, to_fill;

		avail = impl->block_size - impl->filled;
		to_fill = SPA_MIN(avail, size);

		memcpy(&impl->buffer[impl->filled], data, to_fill);

		impl->filled += to_fill;
		avail -= to_fill;
		size -= to_fill;
		data += to_fill;

		if (avail == 0) {
			switch (impl->protocol) {
			case PROTO_UDP:
				flush_to_udp_packet(impl);
				break;
			case PROTO_TCP:
				flush_to_tcp_packet(impl);
				break;
			}
			impl->filled = 0;
		}
	}

	pw_stream_queue_buffer(impl->stream, buf);
}

static int create_udp_socket(struct impl *impl, uint16_t *port)
{
	int res, ip_version, fd, val, i, af;
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;

	if ((res = pw_rtsp_client_get_local_ip(impl->rtsp,
				&ip_version, NULL, 0)) < 0)
		return res;

	if (ip_version == 4) {
		sa4.sin_family = af = AF_INET;
		sa4.sin_addr.s_addr = INADDR_ANY;
	} else {
		sa6.sin6_family = af = AF_INET6;
	        sa6.sin6_addr = in6addr_any;
	}

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

	for (i = 0; i < MAX_PORT_RETRY; i++) {
		int ret;

		if (ip_version == 4) {
			sa4.sin_port = htons(*port);
			ret = bind(fd, &sa4, sizeof(sa4));
		} else {
			sa6.sin6_port = htons(*port);
			ret = bind(fd, &sa6, sizeof(sa6));
		}
		if (ret == 0)
			break;
		if (ret < 0 && errno != EADDRINUSE) {
			res = -errno;
			pw_log_error("bind failed: %m");
			goto error;
		}
		(*port)++;
	}
	return fd;
error:
	close(fd);
	return res;
}

static int connect_socket(struct impl *impl, int type, int fd, uint16_t port)
{
	const char *host;
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	struct sockaddr *sa;
	size_t salen;
	int res, af;

	host = pw_properties_get(impl->props, "raop.hostname");
	if (host == NULL)
		return -EINVAL;

	if (inet_pton(AF_INET, host, &sa4.sin_addr) > 0) {
		sa4.sin_family = af = AF_INET;
		sa4.sin_port = htons(port);
		sa = (struct sockaddr *) &sa4;
		salen = sizeof(sa4);
	} else if (inet_pton(AF_INET6, host, &sa6.sin6_addr) > 0) {
		sa6.sin6_family = af = AF_INET6;
		sa6.sin6_port = htons(port);
		sa = (struct sockaddr *) &sa6;
		salen = sizeof(sa6);
	} else {
		pw_log_error("Invalid host '%s'", host);
		return -EINVAL;
	}

	if (fd < 0 &&
	    (fd = socket(af, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}

	res = connect(fd, sa, salen);
	if (res < 0 && errno != EINPROGRESS) {
		res = -errno;
		pw_log_error("connect failed: %m");
		goto error;
	}
	pw_log_info("Connected to host:%s port:%d", host, port);
	return fd;

error:
	if (fd >= 0)
		close(fd);
	return res;
}

static void
on_timing_source_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	uint32_t packet[8];
	ssize_t bytes;

	if (mask & SPA_IO_IN) {
		uint64_t remote, received;

		received = ntp_now(CLOCK_MONOTONIC);
		bytes = read(impl->timing_fd, packet, sizeof(packet));
		if (bytes < 0) {
			pw_log_debug("error reading timing packet: %m");
			return;
		}
		if (bytes != sizeof(packet)) {
			pw_log_warn("discarding short (%zd < %zd) timing packet",
					bytes, sizeof(bytes));
			return;
		}
		if (packet[0] != ntohl(0x80d20007))
			return;

		remote = ((uint64_t)ntohl(packet[6])) << 32 | ntohl(packet[7]);
		send_udp_timing_packet(impl, remote, received);
	}
}


static void
on_control_source_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	uint32_t packet[2];
	ssize_t bytes;

	if (mask & SPA_IO_IN) {
		uint32_t hdr;
		uint16_t seq, num;

		bytes = read(impl->control_fd, packet, sizeof(packet));
		if (bytes < 0) {
			pw_log_debug("error reading control packet: %m");
			return;
		}
		if (bytes != sizeof(packet)) {
			pw_log_warn("discarding short (%zd < %zd) control packet",
					bytes, sizeof(bytes));
			return;
		}
		hdr = ntohl(packet[0]);
		if ((hdr & 0xff000000) != 0x80000000)
			return;

		seq = ntohl(packet[1]) >> 16;
		num = ntohl(packet[1]) & 0xffff;
		if (num == 0)
			return;

		switch (hdr >> 16 & 0xff) {
		case 0xd5:
			pw_log_debug("retransmit request seq:%u num:%u", seq, num);
			/* retransmit request */
			break;
		}
	}
}

static void rtsp_flush_reply(void *data, int status, const struct spa_dict *headers)
{
	pw_log_info("reply %d", status);
}

static int rtsp_do_flush(struct impl *impl)
{
	int res;

	if (!impl->recording)
		return 0;

	pw_properties_set(impl->headers, "Range", "npt=0-");
	pw_properties_setf(impl->headers, "RTP-Info",
			"seq=%u;rtptime=%u", impl->seq, impl->rtptime);

	impl->recording = false;

	res = pw_rtsp_client_send(impl->rtsp, "FLUSH", &impl->headers->dict,
			NULL, NULL, rtsp_flush_reply, impl);

	pw_properties_set(impl->headers, "Range", NULL);
	pw_properties_set(impl->headers, "RTP-Info", NULL);

	return res;
}

static void rtsp_record_reply(void *data, int status, const struct spa_dict *headers)
{
	struct impl *impl = data;
	const char *str;
	uint32_t n_params;
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_latency_info latency;

	pw_log_info("reply %d", status);

	if ((str = spa_dict_lookup(headers, "Audio-Latency")) != NULL) {
		if (!spa_atou32(str, &impl->latency, 0))
			impl->latency = DEFAULT_LATENCY;
	} else {
		impl->latency = DEFAULT_LATENCY;
	}

	spa_zero(latency);
	latency.direction = PW_DIRECTION_INPUT;
	latency.min_rate = latency.max_rate = impl->latency;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

	pw_stream_update_params(impl->stream, params, n_params);

	impl->first = true;
	impl->sync = 0;
	impl->sync_period = impl->info.rate / (impl->block_size / impl->frame_size);
	impl->recording = true;
}

static int rtsp_do_record(struct impl *impl)
{
	int res;

	if (!impl->ready || impl->recording)
		return 0;

	pw_properties_set(impl->headers, "Range", "npt=0-");
	pw_properties_setf(impl->headers, "RTP-Info",
			"seq=%u;rtptime=%u", impl->seq, impl->rtptime);

	res = pw_rtsp_client_send(impl->rtsp, "RECORD", &impl->headers->dict,
			NULL, NULL, rtsp_record_reply, impl);

	pw_properties_set(impl->headers, "Range", NULL);
	pw_properties_set(impl->headers, "RTP-Info", NULL);

	return res;
}

static void
on_server_source_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP))
		goto error;
	if (mask & SPA_IO_OUT) {
		int res;
		socklen_t len;

		pw_loop_update_io(impl->loop, impl->server_source,
			impl->server_source->mask & ~SPA_IO_OUT);

		len = sizeof(res);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &res, &len) < 0) {
			pw_log_error("getsockopt: %m");
			goto error;
		}
		if (res != 0)
			goto error;

		impl->ready = true;
		if (pw_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING)
			rtsp_do_record(impl);
	}
	return;
error:
	pw_loop_update_io(impl->loop, impl->server_source, 0);
}

static void rtsp_setup_reply(void *data, int status, const struct spa_dict *headers)
{
	struct impl *impl = data;
	const char *str, *state = NULL, *s;
	size_t len;
	uint64_t ntp;
	uint16_t control_port, timing_port;

	pw_log_info("reply %d", status);

	if ((str = spa_dict_lookup(headers, "Session")) == NULL) {
		pw_log_error("missing Session header");
		return;
	}
	pw_properties_set(impl->headers, "Session", str);

	if ((str = spa_dict_lookup(headers, "Transport")) == NULL) {
		pw_log_error("missing Transport header");
		return;
	}

	impl->server_port = control_port = timing_port = 0;
	while ((s = pw_split_walk(str, ";", &len, &state)) != NULL) {
		if (spa_strstartswith(s, "server_port=")) {
			impl->server_port = atoi(s + 12);
		}
		else if (spa_strstartswith(s, "control_port=")) {
			control_port = atoi(s + 13);
		}
		else if (spa_strstartswith(s, "timing_port=")) {
			timing_port = atoi(s + 12);
		}
	}
	if (impl->server_port == 0) {
		pw_log_error("missing server port in Transport");
		return;
	}

	pw_getrandom(&impl->seq, sizeof(impl->seq), 0);
	pw_getrandom(&impl->rtptime, sizeof(impl->rtptime), 0);

	pw_log_info("server port:%u", impl->server_port);

	switch (impl->protocol) {
	case PROTO_TCP:
		if ((impl->server_fd = connect_socket(impl, SOCK_STREAM, -1, impl->server_port)) <= 0)
			return;

		impl->server_source = pw_loop_add_io(impl->loop, impl->server_fd,
				SPA_IO_OUT, false, on_server_source_io, impl);
		break;

	case PROTO_UDP:
		if (control_port == 0 || timing_port == 0) {
			pw_log_error("missing UDP ports in Transport");
			return;
		}
		pw_log_info("control:%u timing:%u", control_port, timing_port);

		if ((impl->server_fd = connect_socket(impl, SOCK_DGRAM, -1, impl->server_port)) <= 0)
			return;
		if ((impl->control_fd = connect_socket(impl, SOCK_DGRAM, impl->control_fd, control_port)) <= 0)
			return;
		if ((impl->timing_fd = connect_socket(impl, SOCK_DGRAM, impl->timing_fd, timing_port)) <= 0)
			return;

		ntp = ntp_now(CLOCK_MONOTONIC);
		send_udp_timing_packet(impl, ntp, ntp);

		impl->timing_source = pw_loop_add_io(impl->loop, impl->timing_fd,
				SPA_IO_IN, false, on_timing_source_io, impl);
		impl->control_source = pw_loop_add_io(impl->loop, impl->control_fd,
				SPA_IO_IN, false, on_control_source_io, impl);

		impl->ready = true;
		if (pw_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING)
			rtsp_do_record(impl);
		break;
	default:
		return;
	}
}

static int rtsp_do_setup(struct impl *impl)
{
	int res;

	switch (impl->protocol) {
	case PROTO_TCP:
		pw_properties_set(impl->headers, "Transport",
				"RTP/AVP/TCP;unicast;interleaved=0-1;mode=record");
		break;

	case PROTO_UDP:
		impl->control_port = DEFAULT_UDP_CONTROL_PORT;
		impl->timing_port = DEFAULT_UDP_TIMING_PORT;

		impl->control_fd = create_udp_socket(impl, &impl->control_port);
		impl->timing_fd = create_udp_socket(impl, &impl->timing_port);
		if (impl->control_fd < 0 || impl->timing_fd < 0)
			goto error;

		pw_properties_setf(impl->headers, "Transport",
				"RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
				"control_port=%u;timing_port=%u",
				impl->control_port, impl->timing_port);
		break;

	default:
		return -ENOTSUP;
	}

	res = pw_rtsp_client_send(impl->rtsp, "SETUP", &impl->headers->dict,
			NULL, NULL, rtsp_setup_reply, impl);

	pw_properties_set(impl->headers, "Transport", NULL);

	return res;
error:
	if (impl->control_fd > 0)
		close(impl->control_fd);
	impl->control_fd = -1;
	if (impl->timing_fd > 0)
		close(impl->timing_fd);
	impl->timing_fd = -1;
	return -EIO;
}

static void rtsp_announce_reply(void *data, int status, const struct spa_dict *headers)
{
	struct impl *impl = data;

	pw_log_info("reply %d", status);

	pw_properties_set(impl->headers, "Apple-Challenge", NULL);

	rtsp_do_setup(impl);
}

static void base64_encode(const uint8_t *data, size_t len, char *enc, char pad)
{
	static const char tab[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;
	for (i = 0; i < len; i += 3) {
		uint32_t v;
		v  =              data[i+0]      << 16;
		v |= (i+1 < len ? data[i+1] : 0) << 8;
		v |= (i+2 < len ? data[i+2] : 0);
		*enc++ =             tab[(v >> (3*6)) & 0x3f];
		*enc++ =             tab[(v >> (2*6)) & 0x3f];
		*enc++ = i+1 < len ? tab[(v >> (1*6)) & 0x3f] : pad;
		*enc++ = i+2 < len ? tab[(v >> (0*6)) & 0x3f] : pad;
	}
	*enc = '\0';
}

static size_t base64_decode(const char *data, size_t len, uint8_t *dec)
{
	uint8_t tab[] = {
		62, -1, -1, -1, 63, 52, 53, 54, 55, 56,
		57, 58, 59, 60, 61, -1, -1, -1, -1, -1,
		-1, -1,  0,  1,  2,  3,  4,  5,  6,  7,
		 8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
		18, 19, 20, 21, 22, 23, 24, 25, -1, -1,
		-1, -1, -1, -1, 26, 27, 28, 29, 30, 31,
		32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
		42, 43, 44, 45, 46, 47, 48, 49, 50, 51 };
	size_t i, j;
	for (i = 0, j = 0; i < len; i += 4) {
		uint32_t v;
		v =                          tab[data[i+0]-43]  << (3*6);
		v |=                         tab[data[i+1]-43]  << (2*6);
		v |= (data[i+2] == '=' ? 0 : tab[data[i+2]-43]) << (1*6);
		v |= (data[i+3] == '=' ? 0 : tab[data[i+3]-43]);
		                      dec[j++] = (v >> 16) & 0xff;
		if (data[i+2] != '=') dec[j++] = (v >> 8)  & 0xff;
		if (data[i+3] != '=') dec[j++] =  v        & 0xff;
	}
	return j;
}

static int rsa_encrypt(uint8_t *data, int len, uint8_t *res)
{
	RSA *rsa;
	uint8_t modulus[256];
	uint8_t exponent[8];
	size_t size;
	BIGNUM *n_bn = NULL;
	BIGNUM *e_bn = NULL;
	char n[] =
		"59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUtwC"
		"5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDR"
		"KSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuB"
		"OitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJ"
		"Q+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEpVviYnh"
		"imNVvYFZeCXg/IdTQ+x4IRdiXNv5hEew==";
	char e[] = "AQAB";

	rsa = RSA_new();

	size = base64_decode(n, strlen(n), modulus);
	n_bn = BN_bin2bn(modulus, size, NULL);

	size = base64_decode(e, strlen(e), exponent);
	e_bn = BN_bin2bn(exponent, size, NULL);

	RSA_set0_key(rsa, n_bn, e_bn, NULL);

	size = RSA_public_encrypt(len, data, res, rsa, RSA_PKCS1_OAEP_PADDING);
	RSA_free(rsa);
	return size;
}

static int rtsp_do_announce(struct impl *impl)
{
	const char *host;
	uint8_t rsakey[512];
	char key[512*2];
	char iv[16*2];
	int res, frames, i, ip_version;
	char *sdp;
        char local_ip[256];

	host = pw_properties_get(impl->props, "raop.hostname");

	if (impl->protocol == PROTO_TCP)
		frames = FRAMES_PER_TCP_PACKET;
	else
		frames = FRAMES_PER_UDP_PACKET;

	impl->block_size = frames * impl->frame_size;

	pw_rtsp_client_get_local_ip(impl->rtsp, &ip_version,
			local_ip, sizeof(local_ip));

	switch (impl->encryption) {
	case CRYPTO_NONE:
		asprintf(&sdp, "v=0\r\n"
				"o=iTunes %s 0 IN IP%d %s\r\n"
				"s=iTunes\r\n"
				"c=IN IP%d %s\r\n"
				"t=0 0\r\n"
				"m=audio 0 RTP/AVP 96\r\n"
				"a=rtpmap:96 AppleLossless\r\n"
				"a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n",
				impl->session_id, ip_version, local_ip,
				ip_version, host, frames);
		break;

	case CRYPTO_RSA:
		pw_getrandom(impl->key, sizeof(impl->key), 0);
		AES_set_encrypt_key(impl->key, 128, &impl->aes);
		pw_getrandom(impl->iv, sizeof(impl->iv), 0);

		i = rsa_encrypt(impl->key, 16, rsakey);
	        base64_encode(rsakey, i, key, '=');
	        base64_encode(impl->iv, 16, iv, '=');

		asprintf(&sdp, "v=0\r\n"
				"o=iTunes %s 0 IN IP%d %s\r\n"
				"s=iTunes\r\n"
				"c=IN IP%d %s\r\n"
				"t=0 0\r\n"
				"m=audio 0 RTP/AVP 96\r\n"
				"a=rtpmap:96 AppleLossless\r\n"
				"a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 44100\r\n"
				"a=rsaaeskey:%s\r\n"
				"a=aesiv:%s\r\n",
				impl->session_id, ip_version, local_ip,
				ip_version, host, frames, key, iv);
		break;
	default:
		return -ENOTSUP;
	}
	res = pw_rtsp_client_send(impl->rtsp, "ANNOUNCE", &impl->headers->dict,
			"application/sdp", sdp, rtsp_announce_reply, impl);
	free(sdp);

	return res;
}

static const char *find_attr(char **tokens, const char *key)
{
	int i;
	char *p, *s;
	for (i = 0; tokens[i]; i++) {
		if (!spa_strstartswith(tokens[i], key))
			continue;
		p = tokens[i] + strlen(key);
		if ((s = rindex(p, '"')) == NULL)
			continue;
		*s = '\0';
		if ((s = index(p, '"')) == NULL)
			continue;
		return s+1;
	}
	return NULL;
}

static void rtsp_auth_reply(void *data, int status, const struct spa_dict *headers)
{
	struct impl *impl = data;
	pw_log_info("auth %d", status);

	switch (status) {
	case 200:
		rtsp_do_announce(impl);
		break;
	}
}

SPA_PRINTF_FUNC(2,3)
static int MD5_hash(char hash[MD5_HASH_LENGTH+1], const char *fmt, ...)
{
	unsigned char d[MD5_DIGEST_LENGTH];
	int i;
	va_list args;
	char buffer[1024];

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	MD5((unsigned char*) buffer, strlen(buffer), d);
	for (i = 0; i < MD5_DIGEST_LENGTH; i++)
		sprintf(&hash[2*i], "%02x", (uint8_t) d[i]);
	hash[MD5_HASH_LENGTH] = '\0';
	return 0;
}

static int rtsp_do_auth(struct impl *impl, const struct spa_dict *headers)
{
	const char *str;
	char **tokens;
	int n_tokens;
	char auth[1024];

	if (impl->password == NULL)
		return -ENOTSUP;

	if ((str = spa_dict_lookup(headers, "WWW-Authenticate")) == NULL)
		return -ENOENT;

	pw_log_info("Auth: %s", str);

	tokens = pw_split_strv(str, " ", INT_MAX, &n_tokens);
	if (tokens == NULL || tokens[0] == NULL)
		goto error;

	if (spa_streq(tokens[0], "Basic")) {
		char buf[256];
		char enc[512];
		spa_scnprintf(buf, sizeof(buf), "%s:%s", DEFAULT_USER_NAME, impl->password);
		base64_encode((uint8_t*)buf, strlen(buf), enc, '=');
		spa_scnprintf(auth, sizeof(auth), "Basic %s", enc);
	}
	else if (spa_streq(tokens[0], "Digest")) {
		const char *realm, *nonce;
		char h1[MD5_HASH_LENGTH+1];
		char h2[MD5_HASH_LENGTH+1];
		char resp[MD5_HASH_LENGTH+1];

		realm = find_attr(tokens, "realm");
		nonce = find_attr(tokens, "nonce");
		if (realm == NULL || nonce == NULL)
			goto error;

		MD5_hash(h1, "%s:%s:%s", DEFAULT_USER_NAME, realm, impl->password);
		MD5_hash(h2, "OPTIONS:*");
		MD5_hash(resp, "%s:%s:%s", h1, nonce, h2);

		spa_scnprintf(auth, sizeof(auth),
				"Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"*\", response=\"%s\"",
				DEFAULT_USER_NAME, realm, nonce, resp);
	}
	else
		return -EINVAL;

	pw_properties_setf(impl->headers, "Authorization", "%s %s",
			tokens[0], auth);
	pw_free_strv(tokens);

	pw_rtsp_client_send(impl->rtsp, "OPTIONS", &impl->headers->dict,
			NULL, NULL, rtsp_auth_reply, impl);

	return 0;
error:
	pw_free_strv(tokens);
	return -EINVAL;
}

static void rtsp_options_reply(void *data, int status, const struct spa_dict *headers)
{
	struct impl *impl = data;
	pw_log_info("options %d", status);

	switch (status) {
	case 401:
		rtsp_do_auth(impl, headers);
		break;
	case 200:
		rtsp_do_announce(impl);
		break;
	}
}

static void rtsp_connected(void *data)
{
	struct impl *impl = data;
	uint32_t sci[2];
	uint8_t rac[16];
	char sac[16*4];

	pw_log_info("connected");

	impl->connected = true;

	pw_getrandom(sci, sizeof(sci), 0);
	pw_properties_setf(impl->headers, "Client-Instance",
			"%08x%08x", sci[0], sci[1]);

	pw_getrandom(rac, sizeof(rac), 0);
	base64_encode(rac, sizeof(rac), sac, '\0');
	pw_properties_set(impl->headers, "Apple-Challenge", sac);

	pw_properties_set(impl->headers, "User-Agent", DEFAULT_USER_AGENT);

	pw_rtsp_client_send(impl->rtsp, "OPTIONS", &impl->headers->dict,
			NULL, NULL, rtsp_options_reply, impl);
}

static void connection_cleanup(struct impl *impl)
{
	impl->ready = false;
	if (impl->server_fd != -1) {
		close(impl->server_fd);
		impl->server_fd = -1;
	}
	if (impl->control_fd != -1) {
		close(impl->control_fd);
		impl->control_fd = -1;
	}
	if (impl->timing_fd != -1) {
		close(impl->timing_fd);
		impl->timing_fd = -1;
	}
	if (impl->server_source != NULL) {
		pw_loop_destroy_source(impl->loop, impl->server_source);
		impl->server_source = NULL;
	}
	if (impl->timing_source != NULL) {
		pw_loop_destroy_source(impl->loop, impl->timing_source);
		impl->timing_source = NULL;
	}
	if (impl->control_source != NULL) {
		pw_loop_destroy_source(impl->loop, impl->control_source);
		impl->control_source = NULL;
	}
}

static void rtsp_disconnected(void *data)
{
	struct impl *impl = data;
	pw_log_info("disconnected");
	impl->connected = false;
	connection_cleanup(impl);
}

static void rtsp_error(void *data, int res)
{
	pw_log_error("error %d", res);
}

static void rtsp_message(void *data, int status,
			const struct spa_dict *headers)
{
	const struct spa_dict_item *it;
	pw_log_info("message %d", status);
	spa_dict_for_each(it, headers)
		pw_log_info(" %s: %s", it->key, it->value);

}

static const struct pw_rtsp_client_events rtsp_events = {
	PW_VERSION_RTSP_CLIENT_EVENTS,
	.connected = rtsp_connected,
	.error = rtsp_error,
	.disconnected = rtsp_disconnected,
	.message = rtsp_message,
};

static void stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
		rtsp_do_flush(impl);
		break;
	case PW_STREAM_STATE_STREAMING:
		rtsp_do_record(impl);
		break;
	default:
		break;
	}
}

static int rtsp_do_connect(struct impl *impl)
{
	const char *hostname, *port;
	uint32_t session_id;

	if (impl->connected) {
		if (!impl->ready)
			return rtsp_do_announce(impl);
		return 0;
	}

	hostname = pw_properties_get(impl->props, "raop.hostname");
	port = pw_properties_get(impl->props, "raop.port");
	if (hostname == NULL || port == NULL)
		return -EINVAL;

	pw_getrandom(&session_id, sizeof(session_id), 0);
	spa_scnprintf(impl->session_id, sizeof(impl->session_id), "%u", session_id);

	return pw_rtsp_client_connect(impl->rtsp, hostname, atoi(port), impl->session_id);
}

static void rtsp_teardown_reply(void *data, int status, const struct spa_dict *headers)
{
	struct impl *impl = data;
	const char *str;

	pw_log_info("reply");

	connection_cleanup(impl);

	if ((str = spa_dict_lookup(headers, "Connection")) != NULL) {
		if (spa_streq(str, "close"))
			pw_rtsp_client_disconnect(impl->rtsp);
	}
}

static int rtsp_do_teardown(struct impl *impl)
{
	if (!impl->ready)
		return 0;

	return pw_rtsp_client_send(impl->rtsp, "TEARDOWN", NULL,
			NULL, NULL, rtsp_teardown_reply, impl);
}

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;

	switch (id) {
	case SPA_PARAM_Format:
		if (param == NULL)
			rtsp_do_teardown(impl);
		else
			rtsp_do_connect(impl);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.process = playback_stream_process
};

static int create_stream(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	impl->stream = pw_stream_new(impl->core, "RAOP sink", impl->stream_props);
	impl->stream_props = NULL;

	if (impl->stream == NULL)
		return -errno;

	pw_stream_add_listener(impl->stream,
			&impl->stream_listener,
			&playback_stream_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->info);

	if ((res = pw_stream_connect(impl->stream,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	impl->headers = pw_properties_new(NULL, NULL);

	impl->rtsp = pw_rtsp_client_new(impl->loop, NULL, 0);
	if (impl->rtsp == NULL)
		return -errno;

	pw_rtsp_client_add_listener(impl->rtsp, &impl->rtsp_listener,
			&rtsp_events, impl);

	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
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
	if (impl->stream)
		pw_stream_destroy(impl->stream);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->rtsp)
		pw_rtsp_client_destroy(impl->rtsp);

	pw_properties_free(impl->headers);
	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);
	free(impl->password);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
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

static int parse_audio_info(struct impl *impl)
{
	struct pw_properties *props = impl->stream_props;
	struct spa_audio_info_raw *info = &impl->info;
	const char *str;

	spa_zero(*info);

	if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) == NULL)
		str = DEFAULT_FORMAT;
	info->format = format_from_name(str, strlen(str));
	switch (info->format) {
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_U8:
		impl->frame_size = 1;
		break;
	case SPA_AUDIO_FORMAT_S16:
		impl->frame_size = 2;
		break;
	case SPA_AUDIO_FORMAT_S24:
		impl->frame_size = 3;
		break;
	case SPA_AUDIO_FORMAT_S24_32:
	case SPA_AUDIO_FORMAT_S32:
	case SPA_AUDIO_FORMAT_F32:
		impl->frame_size = 4;
		break;
	case SPA_AUDIO_FORMAT_F64:
		impl->frame_size = 8;
		break;
	default:
		pw_log_error("unsupported format '%s'", str);
		return -EINVAL;
	}
	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, DEFAULT_RATE);
	if (info->rate == 0) {
		pw_log_error("invalid rate '%s'", str);
		return -EINVAL;
	}
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_CHANNELS)) == NULL)
		str = DEFAULT_CHANNELS;
	info->channels = atoi(str);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) == NULL)
		str = DEFAULT_POSITION;
	parse_position(info, str, strlen(str));
	if (info->channels == 0) {
		pw_log_error("invalid channels '%s'", str);
		return -EINVAL;
	}
	impl->frame_size *= info->channels;

	return 0;
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
	struct pw_properties *props = NULL;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	struct impl *impl;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);
	impl->server_fd = -1;
	impl->control_fd = -1;
	impl->timing_fd = -1;

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->props = props;

	impl->stream_props = pw_properties_new(NULL, NULL);
	if (impl->stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "pipewire.dummy");
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "raop-sink-%u", id);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION,
				pw_properties_get(props, PW_KEY_NODE_NAME));
	if (pw_properties_get(props, PW_KEY_NODE_LATENCY) == NULL)
		pw_properties_set(props, PW_KEY_NODE_LATENCY, "352/44100");

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(impl->stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);

	if ((res = parse_audio_info(impl)) < 0) {
		pw_log_error( "can't parse audio format");
		goto error;
	}

	if ((str = pw_properties_get(props, "raop.transport")) == NULL)
		str = "udp";
	if (spa_streq(str, "udp"))
		impl->protocol = PROTO_UDP;
	else if (spa_streq(str, "tcp"))
		impl->protocol = PROTO_TCP;
	else {
		pw_log_error( "can't handle transport %s", str);
		goto error;
	}

	if ((str = pw_properties_get(props, "raop.encryption.type")) == NULL)
		str = "none";
	if (spa_streq(str, "none"))
		impl->encryption = CRYPTO_NONE;
	else if (spa_streq(str, "RSA"))
		impl->encryption = CRYPTO_RSA;
	else {
		pw_log_error( "can't handle encryption type %s", str);
		goto error;
	}

	if ((str = pw_properties_get(props, "raop.audio.codec")) == NULL)
		str = "PCM";
	if (spa_streq(str, "PCM"))
		impl->codec = CODEC_PCM;
	else {
		pw_log_error( "can't handle codec type %s", str);
		goto error;
	}
	str = pw_properties_get(props, "raop.password");
	impl->password = str ? strdup(str) : NULL;

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
		goto error;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = create_stream(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
