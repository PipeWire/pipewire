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
#include <openssl/evp.h>

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
#include "module-rtp/rtp.h"
#include "module-rtp/stream.h"

/** \page page_module_raop_sink AirPlay Sink
 *
 * Creates a new Sink to stream to an Airplay device.
 *
 * Normally this sink is automatically created with \ref page_module_raop_discover
 * with the right parameters but it is possible to manually create a RAOP sink
 * as well.
 *
 * ## Module Name
 *
 * `libpipewire-module-raop-sink`
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

#define BUFFER_SIZE		(1u<<22)
#define BUFFER_MASK		(BUFFER_SIZE-1)
#define BUFFER_SIZE2		(BUFFER_SIZE>>1)
#define BUFFER_MASK2		(BUFFER_SIZE2-1)

#define FRAMES_PER_TCP_PACKET	4096
#define FRAMES_PER_UDP_PACKET	352

#define RAOP_AUDIO_PORT		6000
#define RAOP_UDP_CONTROL_PORT	6001
#define RAOP_UDP_TIMING_PORT	6002

#define AES_CHUNK_SIZE		16
#ifndef MD5_DIGEST_LENGTH
#define MD5_DIGEST_LENGTH	16
#endif
#define MD5_HASH_LENGTH		(2*MD5_DIGEST_LENGTH)

#define DEFAULT_USER_NAME	"PipeWire"
#define RAOP_AUTH_USER_NAME	"iTunes"

#define MAX_PORT_RETRY		128

#define RAOP_FORMAT		"S16LE"
#define RAOP_STRIDE		(2*DEFAULT_CHANNELS)
#define RAOP_RATE		44100
#define RAOP_LATENCY_MS		250
#define DEFAULT_LATENCY_MS	1500

#define VOLUME_MAX		0.0
#define VOLUME_MIN		-30.0
#define VOLUME_MUTE		-144.0

#define MODULE_USAGE	"( raop.ip=<ip address of host> ) "					\
			"( raop.port=<remote port> ) "						\
			"( raop.name=<name of host> ) "						\
			"( raop.hostname=<hostname of host> ) "					\
			"( raop.transport=<transport, default:udp> ) "				\
			"( raop.encryption.type=<encryption, default:none> ) "			\
			"( raop.audio.codec=PCM ) "						\
			"( raop.password=<password for auth> ) "				\
			"( raop.latency.ms=<min latency in ms, default:"SPA_STRINGIFY(DEFAULT_LATENCY_MS)"> ) "	\
			"( node.latency=<latency as fraction> ) "				\
			"( node.name=<name of the nodes> ) "					\
			"( node.description=<description of the nodes> ) "			\
			"( audio.format=<format, default:"RAOP_FORMAT"> ) "			\
			"( audio.rate=<sample rate, default: "SPA_STRINGIFY(RAOP_RATE)"> ) "			\
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
	struct rtp_stream *stream;

	struct pw_rtsp_client *rtsp;
	struct spa_hook rtsp_listener;
	struct pw_properties *headers;

	char session_id[32];
	char *password;
	char *auth_method;
	char *realm;
	char *nonce;

	unsigned int do_disconnect:1;

	uint8_t aes_key[AES_CHUNK_SIZE]; /* Key for aes-cbc */
	uint8_t aes_iv[AES_CHUNK_SIZE];  /* Initialization vector for cbc */
	EVP_CIPHER_CTX *ctx;

	uint16_t control_port;
	int control_fd;
	struct spa_source *control_source;
	struct spa_source *feedback_timer;

	uint16_t timing_port;
	int timing_fd;
	struct spa_source *timing_source;

	uint16_t server_port;
	int server_fd;
	struct spa_source *server_source;

	uint32_t psamples;
	uint64_t rate;
	uint32_t mtu;
	uint32_t stride;
	uint32_t latency;

	uint32_t ssrc;
	uint32_t sync;
	uint32_t sync_period;
	unsigned int connected:1;
	unsigned int ready:1;
	unsigned int recording:1;

	bool mute;
	float volume;

	struct spa_ringbuffer ring;
	uint8_t buffer[BUFFER_SIZE];

	struct spa_io_position *io_position;

	uint32_t filled;
};

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
	EVP_EncryptInit(impl->ctx, EVP_aes_128_cbc(), impl->aes_key, impl->aes_iv);
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

static int send_udp_sync_packet(struct impl *impl, uint32_t rtptime, unsigned int first)
{
	uint32_t out[3];
	uint32_t latency = impl->latency;
	uint64_t transmitted;
	struct rtp_header header;
	struct iovec iov[2];
	struct msghdr msg;
	int res;

	spa_zero(header);
	header.v = 2;
	if (first)
		header.x = 1;
	header.m = 1;
	header.pt = 84;
	header.sequence_number = 7;
	header.timestamp = htonl(rtptime - latency);

	iov[0].iov_base = &header;
	iov[0].iov_len = 8;

	transmitted = ntp_now();
	out[0] = htonl(transmitted >> 32);
	out[1] = htonl(transmitted & 0xffffffff);
	out[2] = htonl(rtptime);

	iov[1].iov_base = out;
	iov[1].iov_len = sizeof(out);

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	res = sendmsg(impl->control_fd, &msg, MSG_NOSIGNAL);
	if (res < 0) {
		res = -errno;
		pw_log_warn("error sending control packet: %d", res);
	}

	pw_log_debug("raop control sync: first:%d latency:%u now:%"PRIx64" rtptime:%u",
			first, latency, transmitted, rtptime);

	return res;
}

static int send_udp_timing_packet(struct impl *impl, uint64_t remote, uint64_t received,
		struct sockaddr *dest_addr, socklen_t addrlen)
{
	uint32_t out[6];
	uint64_t transmitted;
	struct rtp_header header;
	struct iovec iov[2];
	struct msghdr msg;
	int res;

	spa_zero(header);
	header.v = 2;
	header.pt = 83;
	header.m = 1;

	iov[0].iov_base = &header;
	iov[0].iov_len = 8;

	out[0] = htonl(remote >> 32);
	out[1] = htonl(remote & 0xffffffff);

	out[2] = htonl(received >> 32);
	out[3] = htonl(received & 0xffffffff);
	transmitted = ntp_now();
	out[4] = htonl(transmitted >> 32);
	out[5] = htonl(transmitted & 0xffffffff);

	iov[1].iov_base = out;
	iov[1].iov_len = sizeof(out);

	msg.msg_name = dest_addr;
	msg.msg_namelen = addrlen;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	res = sendmsg(impl->timing_fd, &msg, MSG_NOSIGNAL);
	if (res < 0) {
		res = -errno;
		pw_log_warn("error sending timing packet: %d", res);
	}
	pw_log_debug("raop timing sync: remote:%"PRIx64" received:%"PRIx64" transmitted:%"PRIx64,
			remote, received, transmitted);

	return res;
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

static ssize_t send_packet(int fd, struct msghdr *msg)
{
	ssize_t n;
	n = sendmsg(fd, msg, MSG_NOSIGNAL);
	if (n < 0)
		pw_log_debug("sendmsg() failed: %m");
	return n;
}

static void stream_send_packet(void *data, struct iovec *iov, size_t iovlen)
{
	struct impl *impl = data;
	const size_t max = 8 + impl->mtu;
	uint32_t tcp_pkt[1], out[max], len, n_frames, rtptime;
	struct iovec out_vec[3];
	struct rtp_header *header;
	struct msghdr msg;
	uint8_t *dst;

	if (!impl->recording)
		return;

	header = (struct rtp_header*)iov[0].iov_base;
	if (header->v != 2)
		pw_log_warn("invalid rtp packet version");

	rtptime = htonl(header->timestamp);

	if (header->m || ++impl->sync == impl->sync_period) {
		send_udp_sync_packet(impl, rtptime, header->m);
		impl->sync = 0;
	}

	n_frames = iov[1].iov_len / impl->stride;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = out_vec;
	msg.msg_iovlen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	dst = (uint8_t*)&out[0];

	switch (impl->codec) {
	case CODEC_PCM:
	case CODEC_ALAC:
		len = write_codec_pcm(dst, (void *)iov[1].iov_base, n_frames);
		break;
	default:
		len = 8 + impl->mtu;
		memset(dst, 0, len);
		break;
	}
	if (impl->encryption == CRYPTO_RSA)
		aes_encrypt(impl, dst, len);

	if (impl->protocol == PROTO_TCP) {
		out[0] |= htonl((uint32_t) len + 12);
		tcp_pkt[0] = htonl(0x24000000);
  		out_vec[msg.msg_iovlen++] = (struct iovec) { tcp_pkt, 4 };
	}

	out_vec[msg.msg_iovlen++] = (struct iovec) { header, 12 };
	out_vec[msg.msg_iovlen++] = (struct iovec) { out, len };

	pw_log_debug("raop sending %ld", out_vec[0].iov_len + out_vec[1].iov_len + out_vec[2].iov_len);

	send_packet(impl->server_fd, &msg);
}

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
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

static int rtsp_add_raop_auth_header(struct impl *impl, const char *method)
{
	char auth[1024];

	if (impl->auth_method == NULL)
		return 0;

	if (spa_streq(impl->auth_method, "Basic")) {
		char buf[256];
		char enc[512];
		spa_scnprintf(buf, sizeof(buf), "%s:%s", RAOP_AUTH_USER_NAME, impl->password);
		base64_encode((uint8_t*)buf, strlen(buf), enc, '=');
		spa_scnprintf(auth, sizeof(auth), "Basic %s", enc);
	}
	else if (spa_streq(impl->auth_method, "Digest")) {
		const char *url;
		char h1[MD5_HASH_LENGTH+1];
		char h2[MD5_HASH_LENGTH+1];
		char resp[MD5_HASH_LENGTH+1];

		url = pw_rtsp_client_get_url(impl->rtsp);

		MD5_hash(h1, "%s:%s:%s", RAOP_AUTH_USER_NAME, impl->realm, impl->password);
		MD5_hash(h2, "%s:%s", method, url);
		MD5_hash(resp, "%s:%s:%s", h1, impl->nonce, h2);

		spa_scnprintf(auth, sizeof(auth),
				"username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
				RAOP_AUTH_USER_NAME, impl->realm, impl->nonce, url, resp);
	}
	else
		goto error;

	pw_properties_setf(impl->headers, "Authorization", "%s %s",
			impl->auth_method, auth);

	return 0;
error:
	pw_log_error("error adding raop RSA auth");
	return -EINVAL;
}

static int rtsp_send(struct impl *impl, const char *method,
		const char *content_type, const char *content,
		int (*reply) (void *data, int status, const struct spa_dict *headers, const struct pw_array *content))
{
	int res;

	rtsp_add_raop_auth_header(impl, method);

	res = pw_rtsp_client_send(impl->rtsp, method, &impl->headers->dict,
			content_type, content, reply, impl);
	return res;
}

static int rtsp_log_reply_status(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	pw_log_info("reply status: %d", status);
	return 0;
}

static int rtsp_send_volume(struct impl *impl)
{
	if (!impl->recording)
		return 0;

	char header[128], volstr[64];
	snprintf(header, sizeof(header), "volume: %s\r\n",
			spa_dtoa(volstr, sizeof(volstr), impl->mute ? VOLUME_MUTE : impl->volume));
	return rtsp_send(impl, "SET_PARAMETER", "text/parameters", header, rtsp_log_reply_status);
}

static void rtsp_do_post_feedback(void *data, uint64_t expirations)
{
	struct impl *impl = data;

	pw_rtsp_client_url_send(impl->rtsp, "/feedback", "POST", &impl->headers->dict,
					NULL, NULL, 0, rtsp_log_reply_status, impl);
}

static uint32_t msec_to_samples(struct impl *impl, uint32_t msec)
{
	return msec * impl->rate / 1000;
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
	struct timespec timeout, interval;

	pw_log_info("record status: %d", status);

	timeout.tv_sec = 2;
	timeout.tv_nsec = 0;
	interval.tv_sec = 2;
	interval.tv_nsec = 0;

	if (!impl->feedback_timer)
		impl->feedback_timer = pw_loop_add_timer(impl->loop, rtsp_do_post_feedback, impl);
	pw_loop_update_timer(impl->loop, impl->feedback_timer, &timeout, &interval, false);

	if ((str = spa_dict_lookup(headers, "Audio-Latency")) != NULL) {
		uint32_t l;
		if (spa_atou32(str, &l, 0))
			impl->latency = SPA_MAX(l, impl->latency);
	}

	spa_zero(latency);
	latency.direction = PW_DIRECTION_INPUT;
	latency.min_rate = latency.max_rate = impl->latency + msec_to_samples(impl, RAOP_LATENCY_MS);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

	rtp_stream_update_params(impl->stream, params, n_params);

	rtp_stream_set_first(impl->stream);

	impl->sync = 0;
	impl->sync_period = impl->rate / (impl->mtu / impl->stride);
	impl->recording = true;

	rtsp_send_volume(impl);

	snprintf(progress, sizeof(progress), "progress: %s/%s/%s\r\n", "0", "0", "0");
	return rtsp_send(impl, "SET_PARAMETER", "text/parameters", progress, rtsp_log_reply_status);
}

static int rtsp_do_record(struct impl *impl)
{
	int res;
	uint16_t seq;
	uint32_t rtptime;

	if (!impl->ready || impl->recording)
		return 0;

	seq = rtp_stream_get_seq(impl->stream);
	rtptime = rtp_stream_get_time(impl->stream, &impl->rate);

	pw_properties_set(impl->headers, "Range", "npt=0-");
	pw_properties_setf(impl->headers, "RTP-Info",
			"seq=%u;rtptime=%u", seq, rtptime);

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
		if (rtp_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING)
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
		if (rtp_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING)
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
		impl->control_port = RAOP_UDP_CONTROL_PORT;
		impl->timing_port = RAOP_UDP_TIMING_PORT;

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
	uint32_t rtp_latency;
	char key[512*2];
	char iv[16*2];
	int res, rsa_len, ip_version;
	spa_autofree char *sdp = NULL;
	char local_ip[256];
	host = pw_properties_get(impl->props, "raop.ip");
	rtp_latency = msec_to_samples(impl, RAOP_LATENCY_MS);

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
				ip_version, host, impl->psamples, (uint32_t)impl->rate);
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
				ip_version, host, impl->psamples, (uint32_t)impl->rate,
				rtp_latency);
		if (!sdp)
			return -errno;
		break;

	case CRYPTO_RSA:
	{
		uint8_t rac[16];
		char sac[16*4];

		if ((res = pw_getrandom(rac, sizeof(rac), 0)) < 0 ||
		    (res = pw_getrandom(impl->aes_key, sizeof(impl->aes_key), 0)) < 0 ||
		    (res = pw_getrandom(impl->aes_iv, sizeof(impl->aes_iv), 0)) < 0)
			return res;

		base64_encode(rac, sizeof(rac), sac, '\0');
		pw_properties_set(impl->headers, "Apple-Challenge", sac);

		rsa_len = rsa_encrypt(impl->aes_key, 16, rsakey);
		if (rsa_len < 0)
			return -rsa_len;

		base64_encode(rsakey, rsa_len, key, '=');
		base64_encode(impl->aes_iv, 16, iv, '=');

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
				ip_version, host, impl->psamples, (uint32_t)impl->rate,
				key, iv);
		if (!sdp)
			return -errno;
		break;
	}
	default:
		return -ENOTSUP;
	}

	return rtsp_send(impl, "ANNOUNCE", "application/sdp", sdp, rtsp_announce_reply);
}

static int rtsp_post_auth_setup_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;

	pw_log_info("auth-setup status: %d", status);

	return rtsp_do_announce(impl);
}

static int rtsp_do_post_auth_setup(struct impl *impl)
{
	static const unsigned char content[33] =
		"\x01"
		"\x59\x02\xed\xe9\x0d\x4e\xf2\xbd\x4c\xb6\x8a\x63\x30\x03\x82\x07"
		"\xa9\x4d\xbd\x50\xd8\xaa\x46\x5b\x5d\x8c\x01\x2a\x0c\x7e\x1d\x4e";

	return pw_rtsp_client_url_send(impl->rtsp, "/auth-setup", "POST", &impl->headers->dict,
				       "application/octet-stream", content, sizeof(content),
				       rtsp_post_auth_setup_reply, impl);
}

static int rtsp_options_auth_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res = 0;

	pw_log_info("auth status: %d", status);

	switch (status) {
	case 200:
		if (impl->encryption == CRYPTO_AUTH_SETUP)
			res = rtsp_do_post_auth_setup(impl);
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

static int rtsp_do_options_auth(struct impl *impl, const struct spa_dict *headers)
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

	return rtsp_send(impl, "OPTIONS", NULL, NULL, rtsp_options_auth_reply);
}

static int rtsp_options_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res = 0;

	pw_log_info("options status: %d", status);

	switch (status) {
	case 401:
		res = rtsp_do_options_auth(impl, headers);
		break;
	case 200:
		if (impl->encryption == CRYPTO_AUTH_SETUP)
			res = rtsp_do_post_auth_setup(impl);
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
	int res;

	pw_log_info("connected");

	impl->connected = true;

	if ((res = pw_getrandom(sci, sizeof(sci), 0)) < 0) {
		pw_log_error("error generating random data: %s", spa_strerror(res));
		return;
	}

	pw_properties_setf(impl->headers, "Client-Instance",
			"%08X%08X", sci[0], sci[1]);

	pw_properties_setf(impl->headers, "DACP-ID",
			"%08X%08X", sci[0], sci[1]);

	pw_properties_set(impl->headers, "User-Agent", DEFAULT_USER_NAME "/" PACKAGE_VERSION);

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
	if (impl->feedback_timer != NULL) {
		pw_loop_destroy_source(impl->loop, impl->feedback_timer);
		impl->feedback_timer = NULL;
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

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	impl->stream = NULL;
}

static void stream_state_changed(void *data, bool started, const char *error)
{
	struct impl *impl = data;

	if (error) {
		pw_log_error("stream error: %s", error);
		pw_impl_module_schedule_destroy(impl->module);
		return;
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
	impl->recording = false;

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
				if (impl->mute != mute) {
					impl->mute = mute;
					rtsp_send_volume(impl);
				}
                        }
			spa_pod_builder_prop(&b, SPA_PROP_softMute, 0);
			spa_pod_builder_bool(&b, false);
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
				volume = SPA_CLAMPF(cbrt(volume) * 30 - 30, VOLUME_MIN, VOLUME_MAX);
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

	rtp_stream_set_param(impl->stream, id, param);
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

static const struct rtp_stream_events stream_events = {
	RTP_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.send_packet = stream_send_packet
};

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
		rtp_stream_destroy(impl->stream);
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
	int res = 0;

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

	if ((str = pw_properties_get(props, "raop.transport")) == NULL)
		str = "udp";
	if (spa_streq(str, "udp")) {
		impl->protocol = PROTO_UDP;
		impl->psamples = FRAMES_PER_UDP_PACKET;
	} else if (spa_streq(str, "tcp")) {
		impl->protocol = PROTO_TCP;
		impl->psamples = FRAMES_PER_TCP_PACKET;
	} else {
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

	if ((name = pw_properties_get(props, "raop.name")) == NULL)
		name = "RAOP";

	if ((str = strchr(name, '@')) != NULL) {
		str++;
		if (strlen(str) > 0)
			name = str;
	}
	if ((hostname = pw_properties_get(props, "raop.hostname")) == NULL)
		hostname = name;

	impl->rate = RAOP_RATE;
	impl->latency = msec_to_samples(impl, RAOP_LATENCY_MS);
	impl->stride = RAOP_STRIDE;
	impl->mtu = impl->stride * impl->psamples;
	impl->sync_period = impl->rate / impl->psamples;

	if ((str = pw_properties_get(props, "raop.latency.ms")) == NULL)
		str = SPA_STRINGIFY(DEFAULT_LATENCY_MS);
	impl->latency = SPA_MAX(impl->latency, msec_to_samples(impl, atoi(str)));

	if (pw_properties_get(props, PW_KEY_AUDIO_FORMAT) == NULL)
		pw_properties_setf(props, PW_KEY_AUDIO_FORMAT, "%s", RAOP_FORMAT);
	if (pw_properties_get(props, PW_KEY_AUDIO_RATE) == NULL)
		pw_properties_setf(props, PW_KEY_AUDIO_RATE, "%ld", impl->rate);
	if (pw_properties_get(props, PW_KEY_DEVICE_ICON_NAME) == NULL)
		pw_properties_set(props, PW_KEY_DEVICE_ICON_NAME, "audio-speakers");
	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "raop_sink.%s.%s.%s",
				hostname, ip, port);
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "RAOP to %s", name);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "%s", name);
	if (pw_properties_get(props, PW_KEY_NODE_LATENCY) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%ld",
				impl->psamples, impl->rate);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	if (pw_properties_get(props, PW_KEY_MEDIA_FORMAT) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_FORMAT, "%d", SPA_AUDIO_FORMAT_S16_LE);
	if (pw_properties_get(props, "net.mtu") == NULL)
		pw_properties_setf(props, "net.mtu", "%d", impl->mtu);
	if (pw_properties_get(props, "rtp.sender-ts-offset") == NULL)
		pw_properties_setf(props, "rtp.sender-ts-offset", "%d", 0);
	if (pw_properties_get(props, "sess.ts-direct") == NULL)
		pw_properties_set(props, "sess.ts-direct", 0);
	if (pw_properties_get(props, "sess.media") == NULL)
		pw_properties_set(props, "sess.media", "raop");
	if (pw_properties_get(props, "sess.latency.msec") == NULL)
		pw_properties_setf(props, "sess.latency.msec", "%d", RAOP_LATENCY_MS);

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
	copy_props(impl, props, PW_KEY_MEDIA_FORMAT);
	copy_props(impl, props, PW_KEY_MEDIA_NAME);
	copy_props(impl, props, "net.mtu");
	copy_props(impl, props, "rtp.sender-ts-offset");
	copy_props(impl, props, "sess.media");
	copy_props(impl, props, "sess.name");
	copy_props(impl, props, "sess.min-ptime");
	copy_props(impl, props, "sess.max-ptime");
	copy_props(impl, props, "sess.latency.msec");
	copy_props(impl, props, "sess.ts-refclk");
	copy_props(impl, props, "sess.ts-direct");

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

	impl->stream = rtp_stream_new(impl->core,
			PW_DIRECTION_INPUT, pw_properties_copy(impl->stream_props),
			&stream_events, impl);
	if (impl->stream == NULL) {
		res = -errno;
		pw_log_error("can't create raop stream: %m");
		goto error;
	}

	impl->headers = pw_properties_new(NULL, NULL);

	impl->rtsp = pw_rtsp_client_new(impl->loop, NULL, 0);
	if (impl->rtsp == NULL)
		goto error;

	pw_rtsp_client_add_listener(impl->rtsp, &impl->rtsp_listener,
			&rtsp_events, impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
