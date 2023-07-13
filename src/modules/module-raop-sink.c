/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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
#if OPENSSL_API_LEVEL >= 30000
#include <openssl/core_names.h>
#endif
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
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/latency-utils.h>

#include <pipewire/cleanup.h>
#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include "module-raop/rtsp-client.h"

/** \page page_module_raop_sink PipeWire Module: AirPlay Sink
 *
 * Creates a new Sink to stream to an Airplay device.
 *
 * Normally this sink is automatically created with \ref page_module_raop_discover
 * with the right parameters but it is possible to manually create a RAOP sink
 * as well.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `raop.ip`: The ip address of the remote end.
 * - `raop.port`: The port of the remote end.
 * - `raop.name`: The name of the remote end.
 * - `raop.hostname`: The hostname of the remote end.
 * - `raop.transport`: The data transport to use, one of "udp" or "tcp". Defaults
 *                    to "udp".
 * - `raop.encryption.type`: The encryption type to use. One of "none", "RSA" or
 *                    "auth_setup". Default is "none".
 * - `raop.audio.codec`: The audio codec to use. Needs to be "PCM". Defaults to "PCM".
 * - `raop.password`: The password to use.
 * - `stream.props = {}`: properties to be passed to the sink stream
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-raop-sink
 *     args = {
 *         # Set the remote address to tunnel to
 *         raop.ip = "127.0.0.1"
 *         raop.port = 8190
 *         raop.name = "my-raop-device"
 *         raop.hostname = "My Service"
 *         #raop.transport = "udp"
 *         raop.encryption.type = "RSA"
 *         #raop.audio.codec = "PCM"
 *         #raop.password = "****"
 *         #audio.format = "S16"
 *         #audio.rate = 44100
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *             # extra sink properties
 *         }
 *     }
 * }
 * ]
 *\endcode
 *
 * ## See also
 *
 * \ref page_module_raop_discover
 */

#define NAME "raop-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define FRAMES_PER_TCP_PACKET 4096
#define FRAMES_PER_UDP_PACKET 352

#define RAOP_LATENCY_MIN	11025u
#define DEFAULT_LATENCY_MS	"1500"

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
#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

#define VOLUME_MAX  0.0
#define VOLUME_DEF -30.0
#define VOLUME_MIN -144.0

#define MODULE_USAGE	"( raop.ip=<ip address of host> ) "					\
			"( raop.port=<remote port> ) "						\
			"( raop.name=<name of host> ) "						\
			"( raop.hostname=<hostname of host> ) "					\
			"( raop.transport=<transport, default:udp> ) "				\
			"( raop.encryption.type=<encryption, default:none> ) "			\
			"( raop.audio.codec=PCM ) "						\
			"( raop.password=<password for auth> ) "				\
			"( node.latency=<latency as fraction> ) "				\
			"( node.name=<name of the nodes> ) "					\
			"( node.description=<description of the nodes> ) "			\
			"( audio.format=<format, default:"DEFAULT_FORMAT"> ) "			\
			"( audio.rate=<sample rate, default: "SPA_STRINGIFY(DEFAULT_RATE)"> ) "			\
			"( audio.channels=<number of channels, default:"SPA_STRINGIFY(DEFAULT_CHANNELS)"> ) "	\
			"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "		\
			"( stream.props=<properties> ) "


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
	CRYPTO_AUTH_SETUP,
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
	char *auth_method;
	char *realm;
	char *nonce;

	unsigned int do_disconnect:1;

	uint8_t key[AES_CHUNK_SIZE]; /* Key for aes-cbc */
	uint8_t iv[AES_CHUNK_SIZE];  /* Initialization vector for cbc */
	EVP_CIPHER_CTX *ctx;

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

	bool mute;
	float volume;

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
	int i = len & ~0xf, clen = i;
	EVP_EncryptInit(impl->ctx, EVP_aes_128_cbc(), impl->key, impl->iv);
	EVP_EncryptUpdate(impl->ctx, data, &clen, data, i);
	return i;
}

static inline uint64_t timespec_to_ntp(struct timespec *ts)
{
    uint64_t ntp = (uint64_t) ts->tv_nsec * UINT32_MAX / SPA_NSEC_PER_SEC;
    return ntp | (uint64_t) (ts->tv_sec + 0x83aa7e80) << 32;
}

static inline uint64_t ntp_now(void)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return timespec_to_ntp(&now);
}

static int send_udp_sync_packet(struct impl *impl,
		struct sockaddr *dest_addr, socklen_t addrlen)
{
	uint32_t pkt[5];
	uint32_t rtptime = impl->rtptime;
	uint32_t latency = impl->latency;
	uint64_t transmitted;

	pkt[0] = htonl(0x80d40007);
	if (impl->first)
		pkt[0] |= htonl(0x10000000);
	pkt[1] = htonl(rtptime - latency);
	transmitted = ntp_now();
	pkt[2] = htonl(transmitted >> 32);
	pkt[3] = htonl(transmitted & 0xffffffff);
	pkt[4] = htonl(rtptime);

	pw_log_debug("sync: first:%d latency:%u now:%"PRIx64" rtptime:%u",
			impl->first, latency, transmitted, rtptime);

	return sendto(impl->control_fd, pkt, sizeof(pkt), 0, dest_addr, addrlen);
}

static int send_udp_timing_packet(struct impl *impl, uint64_t remote, uint64_t received,
		struct sockaddr *dest_addr, socklen_t addrlen)
{
	uint32_t pkt[8];
	uint64_t transmitted;

	pkt[0] = htonl(0x80d30007);
	pkt[1] = 0x00000000;
	pkt[2] = htonl(remote >> 32);
	pkt[3] = htonl(remote & 0xffffffff);
	pkt[4] = htonl(received >> 32);
	pkt[5] = htonl(received & 0xffffffff);
	transmitted = ntp_now();
	pkt[6] = htonl(transmitted >> 32);
	pkt[7] = htonl(transmitted & 0xffffffff);

	pw_log_debug("sync: remote:%"PRIx64" received:%"PRIx64" transmitted:%"PRIx64,
			remote, received, transmitted);

	return sendto(impl->timing_fd, pkt, sizeof(pkt), 0, dest_addr, addrlen);
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

	if (impl->first || ++impl->sync == impl->sync_period) {
		impl->sync = 0;
		send_udp_sync_packet(impl, NULL, 0);
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
	case CODEC_ALAC:
		len = write_codec_pcm(dst, impl->buffer, n_frames);
		break;
	default:
		len = 8 + impl->block_size;
		memset(dst, 0, len);
		break;
	}
	if (impl->encryption == CRYPTO_RSA)
		aes_encrypt(impl, dst, len);

	impl->rtptime += n_frames;
	impl->seq = (impl->seq + 1) & 0xffff;

	pw_log_debug("send %u", len + 12);
	res = send(impl->server_fd, pkt, len + 12, 0);

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
	case CODEC_ALAC:
		len = write_codec_pcm(dst, impl->buffer, n_frames);
		break;
	default:
		len = 8 + impl->block_size;
		memset(dst, 0, len);
		break;
	}
	if (impl->encryption == CRYPTO_RSA)
		aes_encrypt(impl, dst, len);

	pkt[0] |= htonl((uint32_t) len + 12);

	impl->rtptime += n_frames;
	impl->seq = (impl->seq + 1) & 0xffff;

	pw_log_debug("send %u", len + 16);
	res = send(impl->server_fd, pkt, len + 16, 0);

	impl->first = false;

	return res;
}

static void playback_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	uint8_t *data;
	uint32_t offs, size;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	bd = &buf->buffer->datas[0];

	offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
	size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);
	data = SPA_PTROFF(bd->data, offs, uint8_t);

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
			ret = bind(fd, (struct sockaddr*)&sa4, sizeof(sa4));
		} else {
			sa6.sin6_port = htons(*port);
			ret = bind(fd, (struct sockaddr*)&sa6, sizeof(sa6));
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

	host = pw_properties_get(impl->props, "raop.ip");
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

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("error on timing socket: %08x", mask);
		pw_loop_update_io(impl->loop, impl->timing_source, 0);
		return;
	}
	if (mask & SPA_IO_IN) {
		uint64_t remote, received;
		struct sockaddr_storage sender;
		socklen_t sender_size = sizeof(sender);

		received = ntp_now();
		bytes = recvfrom(impl->timing_fd, packet, sizeof(packet), 0,
				(struct sockaddr*)&sender, &sender_size);
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
		if (send_udp_timing_packet(impl, remote, received,
				(struct sockaddr *)&sender, sender_size) < 0) {
			pw_log_warn("error sending timing packet");
			return;
		}
	}
}


static void
on_control_source_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	uint32_t packet[2];
	ssize_t bytes;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("error on control socket: %08x", mask);
		pw_loop_update_io(impl->loop, impl->control_source, 0);
		return;
	}
	if (mask & SPA_IO_IN) {
		uint32_t hdr;
		uint16_t seq, num;

		bytes = read(impl->control_fd, packet, sizeof(packet));
		if (bytes < 0) {
			pw_log_warn("error reading control packet: %m");
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

SPA_PRINTF_FUNC(2,3)
static int MD5_hash(char hash[MD5_HASH_LENGTH+1], const char *fmt, ...)
{
	unsigned char d[MD5_DIGEST_LENGTH];
	int i;
	va_list args;
	char buffer[1024];
	unsigned int size;

	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	size = MD5_DIGEST_LENGTH;
	EVP_Digest(buffer, strlen(buffer), d, &size, EVP_md5(), NULL);
	for (i = 0; i < MD5_DIGEST_LENGTH; i++)
		sprintf(&hash[2*i], "%02x", (uint8_t) d[i]);
	hash[MD5_HASH_LENGTH] = '\0';
	return 0;
}

static int rtsp_add_auth(struct impl *impl, const char *method)
{
	char auth[1024];

	if (impl->auth_method == NULL)
		return 0;

	if (spa_streq(impl->auth_method, "Basic")) {
		char buf[256];
		char enc[512];
		spa_scnprintf(buf, sizeof(buf), "%s:%s", DEFAULT_USER_NAME, impl->password);
		base64_encode((uint8_t*)buf, strlen(buf), enc, '=');
		spa_scnprintf(auth, sizeof(auth), "Basic %s", enc);
	}
	else if (spa_streq(impl->auth_method, "Digest")) {
		const char *url;
		char h1[MD5_HASH_LENGTH+1];
		char h2[MD5_HASH_LENGTH+1];
		char resp[MD5_HASH_LENGTH+1];

		url = pw_rtsp_client_get_url(impl->rtsp);

		MD5_hash(h1, "%s:%s:%s", DEFAULT_USER_NAME, impl->realm, impl->password);
		MD5_hash(h2, "%s:%s", method, url);
		MD5_hash(resp, "%s:%s:%s", h1, impl->nonce, h2);

		spa_scnprintf(auth, sizeof(auth),
				"username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
				DEFAULT_USER_NAME, impl->realm, impl->nonce, url, resp);
	}
	else
		goto error;

	pw_properties_setf(impl->headers, "Authorization", "%s %s",
			impl->auth_method, auth);

	return 0;
error:
	pw_log_error("error adding auth");
	return -EINVAL;
}

static int rtsp_send(struct impl *impl, const char *method,
		const char *content_type, const char *content,
		int (*reply) (void *data, int status, const struct spa_dict *headers, const struct pw_array *content))
{
	int res;

	rtsp_add_auth(impl, method);

	res = pw_rtsp_client_send(impl->rtsp, method, &impl->headers->dict,
			content_type, content, reply, impl);
	return res;
}

static int rtsp_log_reply_status(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	pw_log_info("reply status: %d", status);
	return 0;
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

	res = rtsp_send(impl, "FLUSH", NULL, NULL, rtsp_log_reply_status);

	pw_properties_set(impl->headers, "Range", NULL);
	pw_properties_set(impl->headers, "RTP-Info", NULL);

	return res;
}

static int rtsp_send_volume(struct impl *impl)
{
	if (!impl->recording)
		return 0;

	char header[128], volstr[64];
	snprintf(header, sizeof(header), "volume: %s\r\n",
			spa_dtoa(volstr, sizeof(volstr), impl->volume));
	return rtsp_send(impl, "SET_PARAMETER", "text/parameters", header, rtsp_log_reply_status);
}

static int rtsp_record_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	const char *str;
	uint32_t n_params;
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_latency_info latency;
	char progress[128];

	pw_log_info("record status: %d", status);

	if ((str = spa_dict_lookup(headers, "Audio-Latency")) != NULL) {
		uint32_t l;
		if (spa_atou32(str, &l, 0))
			impl->latency = SPA_MAX(l, impl->latency);
	}

	spa_zero(latency);
	latency.direction = PW_DIRECTION_INPUT;
	latency.min_rate = latency.max_rate = impl->latency + RAOP_LATENCY_MIN;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

	pw_stream_update_params(impl->stream, params, n_params);

	impl->first = true;
	impl->sync = 0;
	impl->sync_period = impl->info.rate / (impl->block_size / impl->frame_size);
	impl->recording = true;

	rtsp_send_volume(impl);

	snprintf(progress, sizeof(progress), "progress: %s/%s/%s\r\n", "0", "0", "0");
	return rtsp_send(impl, "SET_PARAMETER", "text/parameters", progress, rtsp_log_reply_status);
}

static int rtsp_do_record(struct impl *impl)
{
	int res;

	if (!impl->ready || impl->recording)
		return 0;

	pw_properties_set(impl->headers, "Range", "npt=0-");
	pw_properties_setf(impl->headers, "RTP-Info",
			"seq=%u;rtptime=%u", impl->seq, impl->rtptime);

	res = rtsp_send(impl, "RECORD", NULL, NULL, rtsp_record_reply);

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

static int rtsp_setup_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	const char *str, *state = NULL, *s;
	size_t len;
	uint64_t ntp;
	uint16_t control_port, timing_port;
	int res;

	pw_log_info("setup status: %d", status);

	if ((str = spa_dict_lookup(headers, "Session")) == NULL) {
		pw_log_error("missing Session header");
		return 0;
	}
	pw_properties_set(impl->headers, "Session", str);

	if ((str = spa_dict_lookup(headers, "Transport")) == NULL) {
		pw_log_error("missing Transport header");
		return 0;
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
		return 0;
	}

	if ((res = pw_getrandom(&impl->seq, sizeof(impl->seq), 0)) < 0 ||
	    (res = pw_getrandom(&impl->rtptime, sizeof(impl->rtptime), 0)) <  0) {
		pw_log_error("error generating random seq and rtptime: %s", spa_strerror(res));
		return 0;
	}

	pw_log_info("server port:%u", impl->server_port);

	switch (impl->protocol) {
	case PROTO_TCP:
		if ((impl->server_fd = connect_socket(impl, SOCK_STREAM, -1, impl->server_port)) < 0)
			return impl->server_fd;

		impl->server_source = pw_loop_add_io(impl->loop, impl->server_fd,
				SPA_IO_OUT, false, on_server_source_io, impl);
		break;

	case PROTO_UDP:
		if (control_port == 0) {
			pw_log_error("missing UDP ports in Transport");
			return 0;
		}
		pw_log_info("control:%u timing:%u", control_port, timing_port);

		if ((impl->server_fd = connect_socket(impl, SOCK_DGRAM, -1, impl->server_port)) < 0)
			return impl->server_fd;
		if ((impl->control_fd = connect_socket(impl, SOCK_DGRAM, impl->control_fd, control_port)) < 0)
			return impl->control_fd;
		if (timing_port != 0) {
			/* it is possible that there is no timing_port. We simply don't
			 * connect then and don't send an initial timing packet.
			 * We will reply to received timing packets on the same address we
			 * received the packet from so we don't really need this. */
			if ((impl->timing_fd = connect_socket(impl, SOCK_DGRAM, impl->timing_fd, timing_port)) < 0)
				return impl->timing_fd;

			ntp = ntp_now();
			send_udp_timing_packet(impl, ntp, ntp, NULL, 0);
		}

		impl->control_source = pw_loop_add_io(impl->loop, impl->control_fd,
				SPA_IO_IN, false, on_control_source_io, impl);

		impl->ready = true;
		if (pw_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING)
			rtsp_do_record(impl);
		break;
	default:
		return 0;
	}
	return 0;
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

		impl->timing_source = pw_loop_add_io(impl->loop, impl->timing_fd,
				SPA_IO_IN, false, on_timing_source_io, impl);

		pw_properties_setf(impl->headers, "Transport",
				"RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
				"control_port=%u;timing_port=%u",
				impl->control_port, impl->timing_port);
		break;

	default:
		return -ENOTSUP;
	}

	res = rtsp_send(impl, "SETUP", NULL, NULL, rtsp_setup_reply);

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

static int rtsp_announce_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;

	pw_log_info("announce status: %d", status);

	pw_properties_set(impl->headers, "Apple-Challenge", NULL);

	return rtsp_do_setup(impl);
}

static inline void swap_bytes(uint8_t *data, size_t size)
{
	int i, j;
	for (i = 0, j = size-1; i < j; i++, j--)
		SPA_SWAP(data[i], data[j]);
}

static int rsa_encrypt(uint8_t *data, int len, uint8_t *enc)
{
	uint8_t modulus[256];
	uint8_t exponent[8];
	size_t msize, esize;
	int res = 0;
	char n[] =
		"59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUtwC"
		"5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDR"
		"KSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuB"
		"OitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJ"
		"Q+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEpVviYnh"
		"imNVvYFZeCXg/IdTQ+x4IRdiXNv5hEew==";
	char e[] = "AQAB";

	msize = base64_decode(n, strlen(n), modulus);
	esize = base64_decode(e, strlen(e), exponent);

#if OPENSSL_API_LEVEL >= 30000
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	OSSL_PARAM params[5];
	int err = 0;
	size_t size;

#if __BYTE_ORDER == __LITTLE_ENDIAN
	swap_bytes(modulus, msize);
	swap_bytes(exponent, esize);
#endif
	params[0] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_N, modulus, msize);
	params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_E, exponent, esize);
	params[2] = OSSL_PARAM_construct_end();

	ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	if (ctx == NULL ||
	    (err = EVP_PKEY_fromdata_init(ctx)) <= 0 ||
	    (err = EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params)) <= 0)
		goto error;

	EVP_PKEY_CTX_free(ctx);

	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_ASYM_CIPHER_PARAM_PAD_MODE,
                                            OSSL_PKEY_RSA_PAD_MODE_OAEP, 0);
	params[1] = OSSL_PARAM_construct_end();

	if ((ctx = EVP_PKEY_CTX_new_from_pkey(NULL, pkey, NULL)) == NULL ||
	    (err = EVP_PKEY_encrypt_init_ex(ctx, params)) <= 0 ||
	    (err = EVP_PKEY_encrypt(ctx, enc, &size, data, len)) <= 0)
		goto error;

	res = size;
done:
	if (ctx)
		EVP_PKEY_CTX_free(ctx);
	if (pkey)
		EVP_PKEY_free(pkey);
	return res;
#else
	RSA *rsa = RSA_new();
	BIGNUM *n_bn = BN_bin2bn(modulus, msize, NULL);
	BIGNUM *e_bn = BN_bin2bn(exponent, esize, NULL);
	if (rsa == NULL || n_bn == NULL || e_bn == NULL)
		goto error;
	RSA_set0_key(rsa, n_bn, e_bn, NULL);
	n_bn = e_bn = NULL;
	if ((res = RSA_public_encrypt(len, data, enc, rsa, RSA_PKCS1_OAEP_PADDING)) <= 0)
		goto error;
done:
	if (rsa != NULL)
		RSA_free(rsa);
	if (n_bn != NULL)
		BN_free(n_bn);
	if (e_bn != NULL)
		BN_free(e_bn);
	return res;
#endif
error:
	ERR_print_errors_fp(stdout);
	res = -EIO;
	goto done;
}

static int rtsp_do_announce(struct impl *impl)
{
	const char *host;
	uint8_t rsakey[512];
	char key[512*2];
	char iv[16*2];
	int res, frames, rsa_len, ip_version;
	spa_autofree char *sdp = NULL;
	char local_ip[256];
	host = pw_properties_get(impl->props, "raop.ip");

	if (impl->protocol == PROTO_TCP)
		frames = FRAMES_PER_TCP_PACKET;
	else
		frames = FRAMES_PER_UDP_PACKET;

	impl->block_size = frames * impl->frame_size;

	pw_rtsp_client_get_local_ip(impl->rtsp, &ip_version,
			local_ip, sizeof(local_ip));

	switch (impl->encryption) {
	case CRYPTO_NONE:
		sdp = spa_aprintf("v=0\r\n"
				"o=iTunes %s 0 IN IP%d %s\r\n"
				"s=iTunes\r\n"
				"c=IN IP%d %s\r\n"
				"t=0 0\r\n"
				"m=audio 0 RTP/AVP 96\r\n"
				"a=rtpmap:96 AppleLossless\r\n"
				"a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 %u\r\n",
				impl->session_id, ip_version, local_ip,
				ip_version, host, frames, impl->info.rate);
		if (!sdp)
			return -errno;
		break;

	case CRYPTO_AUTH_SETUP:
		sdp = spa_aprintf("v=0\r\n"
				"o=iTunes %s 0 IN IP%d %s\r\n"
				"s=iTunes\r\n"
				"c=IN IP%d %s\r\n"
				"t=0 0\r\n"
				"m=audio 0 RTP/AVP 96\r\n"
				"a=rtpmap:96 AppleLossless\r\n"
				"a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 %u\r\n"
				"a=min-latency:%d",
				impl->session_id, ip_version, local_ip,
				ip_version, host, frames, impl->info.rate,
				RAOP_LATENCY_MIN);
		if (!sdp)
			return -errno;
		break;

	case CRYPTO_RSA:
		if ((res = pw_getrandom(impl->key, sizeof(impl->key), 0)) < 0 ||
		    (res = pw_getrandom(impl->iv, sizeof(impl->iv), 0)) < 0)
			return res;

		rsa_len = rsa_encrypt(impl->key, 16, rsakey);
		if (rsa_len < 0)
			return -rsa_len;

	        base64_encode(rsakey, rsa_len, key, '=');
	        base64_encode(impl->iv, 16, iv, '=');

		sdp = spa_aprintf("v=0\r\n"
				"o=iTunes %s 0 IN IP%d %s\r\n"
				"s=iTunes\r\n"
				"c=IN IP%d %s\r\n"
				"t=0 0\r\n"
				"m=audio 0 RTP/AVP 96\r\n"
				"a=rtpmap:96 AppleLossless\r\n"
				"a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 %u\r\n"
				"a=rsaaeskey:%s\r\n"
				"a=aesiv:%s\r\n",
				impl->session_id, ip_version, local_ip,
				ip_version, host, frames, impl->info.rate,
				key, iv);
		if (!sdp)
			return -errno;
		break;
	default:
		return -ENOTSUP;
	}

	return rtsp_send(impl, "ANNOUNCE", "application/sdp", sdp, rtsp_announce_reply);
}

static int rtsp_auth_setup_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;

	pw_log_info("auth-setup status: %d", status);

	return rtsp_do_announce(impl);
}

static int rtsp_do_auth_setup(struct impl *impl)
{
	static const unsigned char content[33] =
		"\x01"
		"\x59\x02\xed\xe9\x0d\x4e\xf2\xbd\x4c\xb6\x8a\x63\x30\x03\x82\x07"
		"\xa9\x4d\xbd\x50\xd8\xaa\x46\x5b\x5d\x8c\x01\x2a\x0c\x7e\x1d\x4e";

	return pw_rtsp_client_url_send(impl->rtsp, "/auth-setup", "POST", &impl->headers->dict,
				       "application/octet-stream", content, sizeof(content),
				       rtsp_auth_setup_reply, impl);
}

static int rtsp_auth_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res = 0;

	pw_log_info("auth status: %d", status);

	switch (status) {
	case 200:
		if (impl->encryption == CRYPTO_AUTH_SETUP)
			res = rtsp_do_auth_setup(impl);
		else
			res = rtsp_do_announce(impl);
		break;
	}
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

static int rtsp_do_auth(struct impl *impl, const struct spa_dict *headers)
{
	const char *str, *realm, *nonce;
	int n_tokens;

	if ((str = spa_dict_lookup(headers, "WWW-Authenticate")) == NULL)
		return -EINVAL;

	if (impl->password == NULL) {
		pw_log_warn("authentication required but no raop.password property was given");
		return -ENOTSUP;
	}

	pw_log_info("Auth: %s", str);

	spa_auto(pw_strv) tokens = pw_split_strv(str, " ", INT_MAX, &n_tokens);
	if (tokens == NULL || tokens[0] == NULL)
		return -EINVAL;

	impl->auth_method = strdup(tokens[0]);

	if (spa_streq(impl->auth_method, "Digest")) {
		realm = find_attr(tokens, "realm");
		nonce = find_attr(tokens, "nonce");
		if (realm == NULL || nonce == NULL)
			return -EINVAL;

		impl->realm = strdup(realm);
		impl->nonce = strdup(nonce);
	}

	return rtsp_send(impl, "OPTIONS", NULL, NULL, rtsp_auth_reply);
}

static int rtsp_options_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res = 0;

	pw_log_info("options status: %d", status);

	switch (status) {
	case 401:
		res = rtsp_do_auth(impl, headers);
		break;
	case 200:
		if (impl->encryption == CRYPTO_AUTH_SETUP)
			res = rtsp_do_auth_setup(impl);
		else
			res = rtsp_do_announce(impl);
		break;
	}
	return res;
}

static void rtsp_connected(void *data)
{
	struct impl *impl = data;
	uint32_t sci[2];
	uint8_t rac[16];
	char sac[16*4];
	int res;

	pw_log_info("connected");

	impl->connected = true;

	if ((res = pw_getrandom(sci, sizeof(sci), 0)) < 0 ||
	    (res = pw_getrandom(rac, sizeof(rac), 0)) < 0) {
		pw_log_error("error generating random data: %s", spa_strerror(res));
		return;
	}

	pw_properties_setf(impl->headers, "Client-Instance",
			"%08x%08x", sci[0], sci[1]);

	base64_encode(rac, sizeof(rac), sac, '\0');
	pw_properties_set(impl->headers, "Apple-Challenge", sac);

	pw_properties_set(impl->headers, "User-Agent", DEFAULT_USER_AGENT);

	pw_rtsp_client_send(impl->rtsp, "OPTIONS", &impl->headers->dict,
			NULL, NULL, rtsp_options_reply, impl);
}

static void connection_cleanup(struct impl *impl)
{
	impl->ready = false;
	if (impl->server_source != NULL) {
		pw_loop_destroy_source(impl->loop, impl->server_source);
		impl->server_source = NULL;
	}
	if (impl->server_fd >= 0) {
		close(impl->server_fd);
		impl->server_fd = -1;
	}
	if (impl->control_source != NULL) {
		pw_loop_destroy_source(impl->loop, impl->control_source);
		impl->control_source = NULL;
	}
	if (impl->control_fd >= 0) {
		close(impl->control_fd);
		impl->control_fd = -1;
	}
	if (impl->timing_source != NULL) {
		pw_loop_destroy_source(impl->loop, impl->timing_source);
		impl->timing_source = NULL;
	}
	if (impl->timing_fd >= 0) {
		close(impl->timing_fd);
		impl->timing_fd = -1;
	}
	free(impl->auth_method);
	impl->auth_method = NULL;
	free(impl->realm);
	impl->realm = NULL;
	free(impl->nonce);
	impl->nonce = NULL;
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
	int res;

	if (impl->connected) {
		if (!impl->ready)
			return rtsp_do_announce(impl);
		return 0;
	}

	hostname = pw_properties_get(impl->props, "raop.ip");
	port = pw_properties_get(impl->props, "raop.port");
	if (hostname == NULL || port == NULL)
		return -EINVAL;

	if ((res = pw_getrandom(&session_id, sizeof(session_id), 0)) < 0)
		return res;

	spa_scnprintf(impl->session_id, sizeof(impl->session_id), "%u", session_id);

	return pw_rtsp_client_connect(impl->rtsp, hostname, atoi(port), impl->session_id);
}

static int rtsp_teardown_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	const char *str;

	pw_log_info("teardown status: %d", status);

	connection_cleanup(impl);

	if ((str = spa_dict_lookup(headers, "Connection")) != NULL) {
		if (spa_streq(str, "close"))
			pw_rtsp_client_disconnect(impl->rtsp);
	}
	return 0;
}

static int rtsp_do_teardown(struct impl *impl)
{
	if (!impl->ready)
		return 0;

	return rtsp_send(impl, "TEARDOWN", NULL, NULL, rtsp_teardown_reply);
}

static void stream_props_changed(struct impl *impl, uint32_t id, const struct spa_pod *param)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[1];
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_prop *prop;

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) == 0) {
				impl->mute = mute;
                        }
			spa_pod_builder_prop(&b, SPA_PROP_softMute, 0);
			spa_pod_builder_bool(&b, impl->mute);
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t i, n_vols;
			float vols[SPA_AUDIO_MAX_CHANNELS], volume;
			float soft_vols[SPA_AUDIO_MAX_CHANNELS];

			if ((n_vols = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vols, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				volume = 0.0f;
				for (i = 0; i < n_vols; i++) {
					volume += vols[i];
					soft_vols[i] = 1.0f;
				}
				volume /= n_vols;
				volume = SPA_CLAMPF(20.0 * log10(volume), VOLUME_MIN, VOLUME_MAX);
				impl->volume = volume;

				rtsp_send_volume(impl);
			}

			spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
			spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
					n_vols, soft_vols);
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_softVolumes:
		case SPA_PROP_softMute:
			break;
		default:
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
	}
	param = spa_pod_builder_pop(&b, &f[0]);

	pw_stream_set_param(impl->stream, id, param);
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
	case SPA_PARAM_Props:
		if (param != NULL)
			stream_props_changed(impl, id, param);
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

	if (impl->ctx)
		EVP_CIPHER_CTX_free(impl->ctx);

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
		return 0;
	}
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
	struct impl *impl;
	const char *str, *name, *hostname, *ip, *port;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);
	impl->server_fd = -1;
	impl->control_fd = -1;
	impl->timing_fd = -1;
	impl->ctx = EVP_CIPHER_CTX_new();
	if (impl->ctx == NULL) {
		res = -errno;
		pw_log_error( "can't create cipher context: %m");
		goto error;
	}
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

	ip = pw_properties_get(props, "raop.ip");
	port = pw_properties_get(props, "raop.port");
	if (ip == NULL || port == NULL) {
		pw_log_error("Missing raop.ip or raop.port");
		res = -EINVAL;
		goto error;
	}

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if (pw_properties_get(props, PW_KEY_DEVICE_ICON_NAME) == NULL)
		pw_properties_set(props, PW_KEY_DEVICE_ICON_NAME, "audio-speakers");

	if ((name = pw_properties_get(props, "raop.name")) == NULL)
		name = "RAOP";

	if ((str = strstr(name, "@"))) {
		str++;
		if (strlen(str) > 0)
			name = str;
	}
	if ((hostname = pw_properties_get(props, "raop.hostname")) == NULL)
		hostname = name;

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "raop_sink.%s.%s.%s",
				hostname, ip, port);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
					"%s", name);
	if (pw_properties_get(props, PW_KEY_NODE_LATENCY) == NULL)
		pw_properties_set(props, PW_KEY_NODE_LATENCY, "352/44100");

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(impl->stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_DEVICE_ICON_NAME);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);

	parse_audio_info(impl->stream_props, &impl->info);

	impl->frame_size = calc_frame_size(&impl->info);
	if (impl->frame_size == 0) {
		pw_log_error("unsupported audio format:%d channels:%d",
				impl->info.format, impl->info.channels);
		res = -EINVAL;
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
		res = -EINVAL;
		goto error;
	}

	if ((str = pw_properties_get(props, "raop.encryption.type")) == NULL)
		str = "none";
	if (spa_streq(str, "none"))
		impl->encryption = CRYPTO_NONE;
	else if (spa_streq(str, "RSA"))
		impl->encryption = CRYPTO_RSA;
	else if (spa_streq(str, "auth_setup"))
		impl->encryption = CRYPTO_AUTH_SETUP;
	else {
		pw_log_error( "can't handle encryption type %s", str);
		res = -EINVAL;
		goto error;
	}

	if ((str = pw_properties_get(props, "raop.audio.codec")) == NULL)
		str = "PCM";
	if (spa_streq(str, "PCM"))
		impl->codec = CODEC_PCM;
	else if (spa_streq(str, "ALAC"))
		impl->codec = CODEC_ALAC;
	else {
		pw_log_error( "can't handle codec type %s", str);
		res = -EINVAL;
		goto error;
	}
	str = pw_properties_get(props, "raop.password");
	impl->password = str ? strdup(str) : NULL;

	if ((str = pw_properties_get(props, "raop.latency.ms")) == NULL)
		str = DEFAULT_LATENCY_MS;
	impl->latency = SPA_MAX(atoi(str) * impl->info.rate / 1000u, RAOP_LATENCY_MIN);

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
