/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <ctype.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
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
#include <plist/plist.h>

#include <openssl/err.h>
#if OPENSSL_API_LEVEL >= 30000
#include <openssl/core_names.h>
#endif
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/aes.h>
#include <openssl/md5.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <spa/utils/cleanup.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include "network-utils.h"
#include "module-raop/rtsp-client.h"
#include "module-raop/base64.h"
#include "module-raop/srp.h"
#include "module-raop/tlv.h"
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
 * - `raop.encryption.type`: The encryption type to use. One of:
 *                    - "none" - No encryption
 *                    - "RSA" - Legacy RSA encryption
 *                    - "auth_setup" - Legacy auth setup
 *                    - "pair_transient" - Transient pairing (3rd party devices, PIN 3939)
 *                    - "pair_homekit" - HomeKit pairing (3rd party with persistent keys)
 *                    - "pair_apple" - Apple pairing (Apple TV, HomePod)
 *                    Default is "none".
 * - `raop.audio.codec`: The audio codec to use. Needs to be "PCM". Defaults to "PCM".
 * - `raop.password`: The password to use.
 * - `raop.latency.ms`: The playback delay on the receiver in milliseconds, i.e.
 *                    how long after sending the audio is played. Lower values
 *                    reduce the delay but need a more reliable network. Values
 *                    below 250 are raised to 250, and a larger Audio-Latency
 *                    requested by the receiver takes precedence. Default is 1500.
 * - `raop.disable-volume`: If set to "true", the module will not send volume commands
 *                    to the remote device. Volume and mute are then applied in software
 *                    instead and the sink is advertised as having no hardware volume
 *                    control. Default is "false".
 * - `raop.auth_key`: Stored authorization key from pair-setup (for pair_homekit/pair_apple).
 *                    Format: 128 hex chars (Ed25519 private key) + 64 hex chars (server public key).
 * - `raop.pairing-pin`: PIN for the one-time pairing that produces `raop.auth_key`.
 *                    For Apple devices (Apple TV/HomePod) this is the code shown on the
 *                    device. For receivers that display their code on screen only when
 *                    asked (LG webOS TVs and similar), set it to the literal "WAIT": the
 *                    module runs POST /pair-pin-start so the code appears, then reads the
 *                    code from `raop.pairing-pin-file` and completes pair-setup on the same
 *                    connection. When set, the module runs pair-setup, logs the resulting
 *                    key, and stops (it does not stream). Set `raop.auth_key` to that value
 *                    and remove `raop.pairing-pin` to connect from then on.
 * - `raop.pairing-pin-file`: File the module polls for the on-screen PIN when
 *                    `raop.pairing-pin=WAIT` (default `/tmp/pipewire-raop-pin`). Write the
 *                    digits shown on the receiver to this file within ~28s.
 * - `raop.auth-key-file`: Optional path to also write the authorization key to after pairing.
 * - `stream.props = {}`: properties to be passed to the sink stream
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_LAYOUT
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_NODE_NETWORK
 * - \ref PW_KEY_MEDIA_CLASS
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-raop-sink.conf
 *
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
 *         #raop.latency.ms = 1500
 *         #raop.disable-volume = "true"
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

PW_LOG_TOPIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

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
#ifndef SHA512_DIGEST_LENGTH
#define SHA512_DIGEST_LENGTH 64
#endif

#define DEFAULT_USER_NAME	"PipeWire"
#define RAOP_AUTH_USER_NAME	"iTunes"
/* Apple receivers on OS 27 and later answer GET /info with 403 unless the
 * User-Agent looks like a recent AirPlay sender ("AirPlay/1.0" is rejected,
 * "AirPlay/380.20.1" is accepted); a version far above any shipping one keeps
 * passing the check. */
#define AP_USER_AGENT		"AirPlay/999.0.0"
#define AP_USER_NAME		"AirPlay"
#define AP_REQUEST_BUFSIZE	4096
#define AP_SRP_USER_NAME	"Pair-Setup"
#define AP_CONTROL_SALT 	"Control-Salt"
#define AP_CONTROL_ENC_INFO "Control-Write-Encryption-Key"
#define AP_CONTROL_DEC_INFO "Control-Read-Encryption-Key"
#define AP_NONCE_LENGTH 12
#define AP_AUTHTAG_LENGTH 16
#define AP_ENCRYPTED_BLOCK_LENGTH_MAX 1024
// For transient pairing the key_len will be 64 bytes, but only 32 are used for
// audio payload encryption. For normal pairing the key is 32 bytes.
#define AP_AUDIO_KEY_LEN 32
#define AP_ED25519_SIGNATURE_LENGTH 64
#define AP_ED25519_PUBLIC_KEY_LENGTH 32
// The stored "private key" is the 32-byte seed followed by the 32-byte public
// key (libsodium layout). OpenSSL's raw Ed25519 private key is just the seed.
#define AP_ED25519_SEED_LENGTH 32
#define AP_ED25519_PRIVATE_KEY_LENGTH 64
#define AP_X25519_KEY_LENGTH 32

// HKDF salt and info strings for pair-verify (HomeKit standard)
#define AP_PAIR_VERIFY_ENC_SALT "Pair-Verify-Encrypt-Salt"
#define AP_PAIR_VERIFY_ENC_INFO "Pair-Verify-Encrypt-Info"

// HKDF salt and info strings for pair-verify (Apple)
#define AP_PAIR_VERIFY_APPLE_SALT "Pair-Verify-AES-Key"
#define AP_PAIR_VERIFY_APPLE_INFO "Pair-Verify-AES-IV"

// Pair-setup encryption salt/info (for step 5/6 of full pairing)
#define AP_PAIR_SETUP_ENC_SALT "Pair-Setup-Encrypt-Salt"
#define AP_PAIR_SETUP_ENC_INFO "Pair-Setup-Encrypt-Info"

// Accessory info (for signing during pair-setup)
#define AP_PAIR_SETUP_ACCESSORY_SIGN_SALT "Pair-Setup-Accessory-Sign-Salt"
#define AP_PAIR_SETUP_ACCESSORY_SIGN_INFO "Pair-Setup-Accessory-Sign-Info"
#define AP_PAIR_SETUP_CONTROLLER_SIGN_SALT "Pair-Setup-Controller-Sign-Salt"
#define AP_PAIR_SETUP_CONTROLLER_SIGN_INFO "Pair-Setup-Controller-Sign-Info"

#define MAX_PORT_RETRY		128

#define RAOP_FORMAT		DEFAULT_RAOP_AUDIO_FORMAT
#define RAOP_STRIDE		(2*DEFAULT_CHANNELS)
#define RAOP_RATE		44100
#define RAOP_LATENCY_MS		250
#define RAOP_LATENCY_MIN	11025	/* Minimum latency in samples (250ms at 44100Hz) */
#define DEFAULT_LATENCY_MS	1500

#define MAX_CHANNELS		SPA_AUDIO_MAX_CHANNELS
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
			"( raop.disable-volume=<disable volume, default:false> ) "		\
			"( raop.auth_key=<stored auth key from pair-setup> ) "				\
			"( raop.pairing-pin=<PIN for one-time Apple device pairing> ) "			\
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
	{ PW_KEY_MODULE_DESCRIPTION, "An AirPlay audio sink" },
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
	CRYPTO_AUTH_SETUP = 4,
	CRYPTO_FP_SAP25 = 5,
	CRYPTO_PAIR_TRANSIENT = 8,       // Transient pairing (3rd party, PIN 3939)
	CRYPTO_PAIR_HOMEKIT = 9,         // HomeKit normal pairing (3rd party with persistent keys)
	CRYPTO_PAIR_APPLE = 10,          // Apple pairing (Apple TV, HomePod)
};

// Helper macro to check if using AirPlay 2 encryption (any pair mode)
#define IS_AP2_ENCRYPTION(e) ((e) == CRYPTO_PAIR_TRANSIENT || (e) == CRYPTO_PAIR_HOMEKIT || (e) == CRYPTO_PAIR_APPLE)
// Helper macro to check if using pair-verify (persistent keys required)
#define IS_PAIR_VERIFY(e) ((e) == CRYPTO_PAIR_HOMEKIT || (e) == CRYPTO_PAIR_APPLE)
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
	struct pw_timer_queue *timer_queue;

	struct spa_hook module_listener;

	int protocol;
	int encryption;
	int codec;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	/* Watch the "default" metadata for a runtime pairing PIN (see the
	 * on-screen-PIN pairing block); a UI, session manager, or `pw-metadata`
	 * supplies it by setting the "raop.pairing-pin" key. */
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct pw_metadata *metadata;
	struct spa_hook metadata_listener;
	uint32_t metadata_id;
	bool pin_waiting;

	struct pw_properties *stream_props;
	struct rtp_stream *stream;

	struct pw_rtsp_client *rtsp;
	struct spa_hook rtsp_listener;
	struct pw_properties *headers;

	char session_id[32];
	char session_uuid[37];
	char *password;
	char *auth_method;
	char *realm;
	char *nonce;

	unsigned int do_disconnect:1;
	unsigned int auth_setup_auth_retry:1;

	bool disable_volume;

	uint8_t aes_key[AES_CHUNK_SIZE]; /* Key for aes-cbc */
	uint8_t aes_iv[AES_CHUNK_SIZE];  /* Initialization vector for cbc */
	EVP_CIPHER_CTX *ctx;

	struct SRPUser *srp_user;
	uint8_t public_key[32];
	uint8_t private_key[64];
	const uint8_t *pkA;
	int pkA_len;
	uint8_t *pkB;
	uint64_t pkB_len;
	const uint8_t *M1;
	int M1_len;
	uint8_t *M2;
	int M2_len;
	uint8_t *salt;
	uint64_t salt_len;
	uint8_t shared_secret[64];
	size_t shared_secret_len; // Will be 32 (normal) or 64 (transient)
	struct pw_rtsp_cipher_context *cipher_ctx;
	uint64_t audio_nonce; // Monotonic per-packet AEAD nonce for AP2 audio

	// Pair-verify state
	uint8_t client_public_key[AP_ED25519_PUBLIC_KEY_LENGTH];
	uint8_t client_private_key[AP_ED25519_PRIVATE_KEY_LENGTH];
	uint8_t server_public_key[AP_ED25519_PUBLIC_KEY_LENGTH];
	bool have_server_public_key;
	uint8_t client_eph_public_key[AP_X25519_KEY_LENGTH];
	uint8_t client_eph_private_key[AP_X25519_KEY_LENGTH];
	uint8_t server_eph_public_key[AP_X25519_KEY_LENGTH];
	char *auth_key;  // Stored authorization key from pair-setup
	char *pairing_pin;  // PIN for initial (non-transient) pairing; the literal "WAIT" means run /pair-pin-start and read the on-screen code from pairing_pin_file
	char *pairing_pin_file;  // File polled for the on-screen PIN when pairing_pin == "WAIT"
	int pin_wait_ticks;  // remaining 1s poll attempts while waiting for the on-screen PIN
	char device_id[24];  // Our device ID

	uint16_t control_port;
	int control_fd;
	struct spa_source *control_source;
	struct pw_timer feedback_timer;
	struct pw_timer handshake_timer;
	struct pw_timer pin_timer;

	uint16_t timing_port;
	int timing_fd;
	struct spa_source *timing_source;

	uint16_t events_port;
	uint16_t data_port;
	uint16_t server_port;
	int server_fd;
	struct spa_source *server_source;

	uint32_t psamples;
	uint32_t rate;
	uint32_t mtu;
	uint32_t stride;
	uint32_t latency;

	uint32_t ssrc;
	uint32_t sync;
	uint32_t sync_period;
	unsigned int connected:1;
	unsigned int ready:1;
	unsigned int recording:1;
	unsigned int volume_valid:1;
	unsigned int destroying:1;

	bool mute;
	float volume;
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

/* Encrypt RTP audio payload using ChaCha20-Poly1305 for AirPlay 2.
 * The nonce is a 4-byte zero prefix + an 8-byte little-endian per-packet
 * counter; that same 8-byte counter is transmitted as the packet's nonce
 * suffix so the receiver can decrypt. A monotonic 64-bit counter is used
 * (rather than the 16-bit RTP sequence number, which repeats every 65536
 * packets) to guarantee the (key, nonce) pair is never reused.
 * The AAD is bytes 4-11 of the RTP header (timestamp and SSRC, 8 bytes)
 * The auth tag is appended to the ciphertext */
static int rtp_ap2_encrypt(struct impl *impl, uint8_t *data, int len,
			   const struct rtp_header *header, uint8_t *tag,
			   uint64_t nonce_counter)
{
	EVP_CIPHER_CTX *ctx;
	uint8_t nonce[AP_NONCE_LENGTH] = { 0 };
	int outlen;

	memcpy(nonce + 4, &nonce_counter, sizeof(nonce_counter));

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return -1;

	if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL,
			       impl->shared_secret, nonce) != 1)
		goto error;

	if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1)
		goto error;

	/* AAD is timestamp and SSRC (bytes 4-11 of RTP header, 8 bytes) */
	if (EVP_EncryptUpdate(ctx, NULL, &outlen, ((const uint8_t *)header) + 4, 8) != 1)
		goto error;

	/* Encrypt the payload in-place */
	if (EVP_EncryptUpdate(ctx, data, &outlen, data, len) != 1)
		goto error;

	if (EVP_EncryptFinal_ex(ctx, NULL, &outlen) != 1)
		goto error;

	/* Get the auth tag */
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, AP_AUTHTAG_LENGTH, tag) != 1)
		goto error;

	EVP_CIPHER_CTX_free(ctx);
	return 0;

error:
	EVP_CIPHER_CTX_free(ctx);
	return -1;
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
		pw_log_warn("error sending control packet: %d (%m)", res);
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

static int write_codec_pcm(void *dst, size_t max, const void *data, size_t len)
{
	uint8_t *bp, *b;
	int bpos = 0;
	uint32_t i, n_frames;

	b = bp = dst;

	n_frames = len / 4;
	if (n_frames*4 + 8 > max)
		return -ENOSPC;

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

	const uint8_t *d = data;
	for (i = 0; i < n_frames; i++) {
		bit_writer(&bp, &bpos, *(d + 1), 8);
		bit_writer(&bp, &bpos, *(d + 0), 8);
		bit_writer(&bp, &bpos, *(d + 3), 8);
		bit_writer(&bp, &bpos, *(d + 2), 8);
		d += 4;
	}
	bit_writer(&bp, &bpos, 7, 3); /* end tag */
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

static void stream_send_packet(void *data, struct rtp_packet *packet)
{
	struct impl *impl = data;
	const size_t max = 8 + impl->mtu + AP_AUTHTAG_LENGTH + 8; /* extra 8 for nonce */
	uint32_t tcp_pkt[1], out[max], len, rtptime, hlen;
	struct iovec out_vec[5];
	struct rtp_header *header;
	struct msghdr msg;
	uint8_t *dst, *payload;
	size_t payload_size;
	int res;
	uint8_t auth_tag[AP_AUTHTAG_LENGTH];
	uint8_t nonce_suffix[8] = { 0 };
	uint64_t nonce_counter = 0;

	if (!impl->recording)
		return;

	header = (struct rtp_header*)packet->data;
	if (header->v != 2)
		pw_log_warn("invalid rtp packet version");

	hlen = 12 + header->cc * 4;
	if (header->x) {
		if (hlen + 4 > (ssize_t)packet->size)
			return;
		hlen += 4 + ntohs(*SPA_PTROFF(packet->data, hlen + 2, uint16_t)) * 4;
	}
	if (hlen > (ssize_t)packet->size)
		return;

	payload = SPA_PTROFF(packet->data, hlen, uint8_t);
	payload_size = packet->size - hlen;

	rtptime = htonl(header->timestamp);

	if (header->m || ++impl->sync == impl->sync_period) {
		send_udp_sync_packet(impl, rtptime, header->m);
		impl->sync = 0;
	}

	dst = (uint8_t*)&out[0];

	switch (impl->codec) {
	case CODEC_PCM:
	case CODEC_ALAC:
		res = write_codec_pcm(dst, max, payload, payload_size);
		if (res < 0) {
			pw_log_warn("can't write data: %d (%s)", res, spa_strerror(res));
			return;
		}
		len = res;
		break;
	default:
		len = 8 + impl->mtu;
		memset(dst, 0, len);
		break;
	}
	if (impl->encryption == CRYPTO_RSA) {
		aes_encrypt(impl, dst, len);
	} else if (IS_AP2_ENCRYPTION(impl->encryption)) {
		nonce_counter = impl->audio_nonce++;
		if (rtp_ap2_encrypt(impl, dst, len, header, auth_tag, nonce_counter) < 0) {
			pw_log_error("RTP encryption failed");
			return;
		}
	}


	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = out_vec;
	msg.msg_iovlen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	if (impl->protocol == PROTO_TCP) {
		tcp_pkt[0] = htonl(0x24000000 | (len + 12));
		out_vec[msg.msg_iovlen++] = (struct iovec) { tcp_pkt, 4 };
	} else {
		out_vec[2].iov_len = 0;
	}

	out_vec[msg.msg_iovlen++] = (struct iovec) { header, hlen };
	out_vec[msg.msg_iovlen++] = (struct iovec) { out, len };

	/* Append auth tag and nonce suffix for AirPlay 2 encrypted packets.
	 * The 8-byte nonce counter is transmitted so the receiver can decrypt. */
	if (IS_AP2_ENCRYPTION(impl->encryption)) {
		memcpy(nonce_suffix, &nonce_counter, sizeof(nonce_counter));
		out_vec[msg.msg_iovlen++] = (struct iovec) { auth_tag, AP_AUTHTAG_LENGTH };
		out_vec[msg.msg_iovlen++] = (struct iovec) { nonce_suffix, sizeof(nonce_suffix) };
	}

	pw_log_debug("raop sending %zu", out_vec[0].iov_len + out_vec[1].iov_len + out_vec[2].iov_len);

	send_packet(impl->server_fd, &msg);
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
	struct sockaddr_storage addr;
	socklen_t len = 0;
	int res;

	host = pw_properties_get(impl->props, "raop.ip");
	if (host == NULL)
		return -EINVAL;

	if ((res = pw_net_parse_address(host, port, &addr, &len)) < 0) {
		pw_log_error("Invalid host '%s' port:%d", host, port);
		return -EINVAL;
	}
	if (fd < 0 &&
	    (fd = socket(addr.ss_family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}

	res = connect(fd, (struct sockaddr*)&addr, len);
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
					bytes, sizeof(packet));
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
					bytes, sizeof(packet));
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
			rtp_stream_resend_packets(impl->stream, seq, num);
			break;
		}
	}
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
	explicit_bzero(buffer, sizeof(buffer));
	for (i = 0; i < MD5_DIGEST_LENGTH; i++)
		snprintf(&hash[2*i], 3, "%02x", (uint8_t) d[i]);
	hash[MD5_HASH_LENGTH] = '\0';
	return 0;
}

static int rtsp_add_raop_auth_header(struct impl *impl, const char *method, const char *url)
{
	char auth[1024];

	if (impl->auth_method == NULL) {
		pw_properties_set(impl->headers, "Authorization", NULL);
		return 0;
	}

	if (spa_streq(impl->auth_method, "Basic")) {
		char buf[256];
		char enc[512];
		if (impl->password == NULL)
			goto error;
		spa_scnprintf(buf, sizeof(buf), "%s:%s", RAOP_AUTH_USER_NAME, impl->password);
		pw_base64_encode((uint8_t*)buf, strlen(buf), enc, '=');
		explicit_bzero(buf, sizeof(buf));
		spa_scnprintf(auth, sizeof(auth), "%s", enc);
		explicit_bzero(enc, sizeof(enc));
	}
	else if (spa_streq(impl->auth_method, "Digest")) {
		char h1[MD5_HASH_LENGTH+1];
		char h2[MD5_HASH_LENGTH+1];
		char resp[MD5_HASH_LENGTH+1];

		if (url == NULL || impl->realm == NULL || impl->nonce == NULL || impl->password == NULL)
			goto error;

		MD5_hash(h1, "%s:%s:%s", RAOP_AUTH_USER_NAME, impl->realm, impl->password);
		MD5_hash(h2, "%s:%s", method, url);
		MD5_hash(resp, "%s:%s:%s", h1, impl->nonce, h2);

		spa_scnprintf(auth, sizeof(auth),
				"username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
				RAOP_AUTH_USER_NAME, impl->realm, impl->nonce, url, resp);
		explicit_bzero(h1, sizeof(h1));
		explicit_bzero(h2, sizeof(h2));
		explicit_bzero(resp, sizeof(resp));
	}
	else
		goto error;

	pw_properties_setf(impl->headers, "Authorization", "%s %s",
			impl->auth_method, auth);
	explicit_bzero(auth, sizeof(auth));

	return 0;
error:
	pw_log_error("error adding raop auth");
	return -EINVAL;
}

static int rtsp_send_url(struct impl *impl, const char *url, const char *method,
		const char *content_type, const void *content, size_t content_length,
		int (*reply) (void *data, int status, const struct spa_dict *headers, const struct pw_array *content))
{
	int res;

	if ((res = rtsp_add_raop_auth_header(impl, method, url)) < 0)
		return res;

	return pw_rtsp_client_url_send(impl->rtsp, url, method, &impl->headers->dict,
			content_type, content, content_length, reply, impl);
}

static int rtsp_send(struct impl *impl, const char *method,
		const char *content_type, const char *content,
		int (*reply) (void *data, int status, const struct spa_dict *headers, const struct pw_array *content))
{
	const char *url = pw_rtsp_client_get_url(impl->rtsp);
	const size_t content_length = content ? strlen(content) : 0;

	return rtsp_send_url(impl, url, method, content_type, content, content_length, reply);
}

static int rtsp_log_reply_status(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	pw_log_info("reply status: %d", status);
	return 0;
}

static int rtsp_send_volume(struct impl *impl)
{
	if (!impl->recording || !impl->volume_valid || impl->disable_volume)
		return 0;

	char header[128], volstr[64];
	snprintf(header, sizeof(header), "volume: %s\r\n",
			spa_dtoa(volstr, sizeof(volstr), impl->mute ? VOLUME_MUTE : impl->volume));
	return rtsp_send(impl, "SET_PARAMETER", "text/parameters", header, rtsp_log_reply_status);
}

static void rtsp_do_post_feedback(void *data)
{
	struct impl *impl = data;

	pw_rtsp_client_url_send(impl->rtsp, "/feedback", "POST", &impl->headers->dict,
					NULL, NULL, 0, rtsp_log_reply_status, impl);

	pw_timer_queue_add(impl->timer_queue, &impl->feedback_timer,
			&impl->feedback_timer.timeout, 2 * SPA_NSEC_PER_SEC,
			rtsp_do_post_feedback, impl);
}

static uint32_t msec_to_samples(struct impl *impl, uint32_t msec)
{
	return (uint64_t) msec * impl->rate / 1000;
}

static int rtsp_record_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	const char *str;
	char progress[128];
	struct spa_process_latency_info process_latency;

	pw_log_info("record status: %d", status);
	switch (status) {
	case 200:
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}

	/* Handshake completed. */
	pw_timer_queue_cancel(&impl->handshake_timer);

	// feedback timer is only needed for auth_setup	encryption
	if (impl->encryption == CRYPTO_FP_SAP25) {
		pw_timer_queue_cancel(&impl->feedback_timer);
		pw_timer_queue_add(impl->timer_queue, &impl->feedback_timer,
				NULL, 2 * SPA_NSEC_PER_SEC,
				rtsp_do_post_feedback, impl);
	}

	if ((str = spa_dict_lookup(headers, "Audio-Latency")) != NULL) {
		uint32_t l;
		if (spa_atou32(str, &l, 0))
			impl->latency = SPA_MAX(l, impl->latency);
	}
	spa_zero(process_latency);
	process_latency.rate = impl->latency + msec_to_samples(impl, RAOP_LATENCY_MS);

	rtp_stream_update_process_latency(impl->stream, &process_latency);

	rtp_stream_set_first(impl->stream);

	impl->sync = 0;
	impl->recording = true;

	rtsp_send_volume(impl);

	snprintf(progress, sizeof(progress), "progress: %s/%s/%s\r\n", "0", "0", "0");
	return rtsp_send(impl, "SET_PARAMETER", "text/parameters", progress, rtsp_log_reply_status);
}

static int rtsp_ap2_setup_session_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content);
static int rtsp_ap2_setup_stream_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content);

static int rtsp_do_ap2_record(struct impl *impl);
static int rtsp_do_ap2_setpeers(struct impl *impl);
static int rtsp_do_ap2_setup_stream(struct impl *impl);
static int rtsp_do_ap2_pair_setup3(struct impl *impl);
static void derive_device_id(struct impl *impl);

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
		if (rtp_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING) {
			if (IS_AP2_ENCRYPTION(impl->encryption))
				rtsp_do_ap2_record(impl);
			else
				rtsp_do_record(impl);
		}
	}
	return;
error:
	pw_loop_update_io(impl->loop, impl->server_source, 0);
	pw_impl_module_schedule_destroy(impl->module);
}

static int rtsp_raop_setup_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	const char *str, *state = NULL, *s;
	size_t len;
	uint64_t ntp;
	uint16_t control_port, timing_port;

	pw_log_info("setup status: %d", status);
	switch (status) {
	case 200:
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}

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
		unsigned long v;
		if (spa_strstartswith(s, "server_port=")) {
			v = strtoul(s + 12, NULL, 10);
			if (v > 0 && v <= 65535)
				impl->server_port = (uint16_t)v;
		}
		else if (spa_strstartswith(s, "control_port=")) {
			v = strtoul(s + 13, NULL, 10);
			if (v > 0 && v <= 65535)
				control_port = (uint16_t)v;
		}
		else if (spa_strstartswith(s, "timing_port=")) {
			v = strtoul(s + 12, NULL, 10);
			if (v > 0 && v <= 65535)
				timing_port = (uint16_t)v;
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
		if (impl->server_source == NULL)
			return -errno;
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
		if (impl->control_source == NULL)
			return -errno;

		impl->ready = true;
		if (rtp_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING)
			rtsp_do_record(impl);
		break;
	default:
		return 0;
	}
	return 0;
}


static void wplist_dict_add_uint(plist_t node, const char *key, uint64_t val)
{
	plist_t add = plist_new_uint(val);
	plist_dict_set_item(node, key, add);
}

static void wplist_dict_add_string(plist_t node, const char *key, const char *val)
{
	plist_t add = plist_new_string(val);
	plist_dict_set_item(node, key, add);
}

static void wplist_dict_add_bool(plist_t node, const char *key, bool val)
{
	plist_t add = plist_new_bool(val);
	plist_dict_set_item(node, key, add);
}

static void wplist_dict_add_data(plist_t node, const char *key, uint8_t *data, size_t len)
{
	plist_t add = plist_new_data((const char *)data, len);
	plist_dict_set_item(node, key, add);
}

static int
wplist_to_bin(uint8_t **data, size_t *len, plist_t node)
{
	char *out = NULL;
	uint32_t out_len = 0;

	plist_to_bin(node, &out, &out_len);
	if (!out)
		return -1;

	*data = (uint8_t *)out;
	*len = out_len;

	return 0;
}

static int
wplist_from_bin(plist_t *node, const struct pw_array *buf)
{
  plist_t out = NULL;

  plist_from_bin((char *)buf->data, (uint32_t)buf->size, &out);
  if (!out)
    return -1;

  *node = out;

  return 0;
}

static int rtsp_ap2_record_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	const char *str;
	char progress[128];
	struct spa_process_latency_info process_latency;

	pw_log_info("AP2 record status: %d", status);
	switch (status) {
	case 200:
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}

	if ((str = spa_dict_lookup(headers, "Audio-Latency")) != NULL) {
		uint32_t l;
		if (spa_atou32(str, &l, 0))
			impl->latency = SPA_MAX(l, impl->latency);
	}
	spa_zero(process_latency);
	process_latency.rate = impl->latency + msec_to_samples(impl, RAOP_LATENCY_MS);

	rtp_stream_update_process_latency(impl->stream, &process_latency);
	rtp_stream_set_first(impl->stream);

	impl->sync = 0;
	impl->recording = true;

	/* Handshake completed. */
	pw_timer_queue_cancel(&impl->handshake_timer);

	/* AirPlay 2 receivers drop idle sessions; keep the (encrypted) control
	 * channel alive with the periodic, self-rearming /feedback POST, the same
	 * mechanism the SAP25 path uses. connection_cleanup cancels it on teardown. */
	pw_timer_queue_cancel(&impl->feedback_timer);
	pw_timer_queue_add(impl->timer_queue, &impl->feedback_timer,
			NULL, 2 * SPA_NSEC_PER_SEC, rtsp_do_post_feedback, impl);

	rtsp_send_volume(impl);

	snprintf(progress, sizeof(progress), "progress: %s/%s/%s\r\n", "0", "0", "0");
	return rtsp_send(impl, "SET_PARAMETER", "text/parameters", progress, rtsp_log_reply_status);
}

static int rtsp_do_ap2_record(struct impl *impl)
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

	res = rtsp_send(impl, "RECORD", NULL, NULL, rtsp_ap2_record_reply);

	pw_properties_set(impl->headers, "Range", NULL);
	pw_properties_set(impl->headers, "RTP-Info", NULL);

	return res;
}

static int rtsp_ap2_setpeers_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;

	pw_log_info("AP2 setpeers status: %d", status);
	switch (status) {
	case 200:
		break;
	default:
		pw_log_warn("SETPEERS failed with status %d, continuing anyway", status);
		break;
	}

	return rtsp_do_ap2_setup_stream(impl);
}

static int rtsp_do_ap2_setpeers(struct impl *impl)
{
	plist_t root;
	plist_t item;
	uint8_t *plist_bin = NULL;
	size_t plist_len = 0;
	int ip_version;
	char local_ip[INET6_ADDRSTRLEN];
	const char *host;
	int res;

	pw_rtsp_client_get_local_ip(impl->rtsp, &ip_version, local_ip, sizeof(local_ip));
	host = pw_properties_get(impl->props, "raop.ip");

	root = plist_new_array();
	item = plist_new_string(host);
	plist_array_append_item(root, item);
	item = plist_new_string(local_ip);
	plist_array_append_item(root, item);

	if (wplist_to_bin(&plist_bin, &plist_len, root) < 0) {
		pw_log_error("Failed to serialize SETPEERS plist");
		plist_free(root);
		return -1;
	}

	res = pw_rtsp_client_url_send(impl->rtsp, "/peer-list-changed", "SETPEERS", &impl->headers->dict,
				"application/x-apple-binary-plist", plist_bin, plist_len,
				rtsp_ap2_setpeers_reply, impl);

	plist_free(root);
	free(plist_bin);

	return res;
}

/* Read a TCP/UDP port from a plist dict, validating type and range. libplist
 * leaves the output untouched when the node is not a PLIST_UINT, so reading
 * without a type check would use a stale/uninitialised value chosen indirectly
 * by the (untrusted) device. Returns true and stores the port on success. */
static bool plist_dict_get_port(plist_t dict, const char *key, uint16_t *out)
{
	plist_t item = plist_dict_get_item(dict, key);
	uint64_t v = 0;

	if (item == NULL || plist_get_node_type(item) != PLIST_UINT)
		return false;
	plist_get_uint_val(item, &v);
	if (v == 0 || v > 65535)
		return false;
	*out = (uint16_t)v;
	return true;
}

static int rtsp_ap2_setup_stream_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	plist_t response;
	plist_t streams;
	plist_t stream;
	int ret;

	pw_log_info("AP2 setup stream status: %d", status);
	switch (status) {
	case 200:
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}

	ret = wplist_from_bin(&response, content);
	if (ret < 0) {
		pw_log_error("Could not parse plist from stream SETUP response");
		return -1;
	}

	streams = plist_dict_get_item(response, "streams");
	if (!streams) {
		pw_log_error("Missing streams in stream SETUP response");
		plist_free(response);
		return -1;
	}

	stream = plist_array_get_item(streams, 0);
	if (!stream) {
		pw_log_error("Missing stream item in response");
		plist_free(response);
		return -1;
	}

	if (!plist_dict_get_port(stream, "dataPort", &impl->data_port)) {
		pw_log_error("Missing or invalid data port in stream SETUP response");
		plist_free(response);
		return -1;
	}

	/* controlPort is optional; leave impl->control_port unchanged if absent. */
	plist_dict_get_port(stream, "controlPort", &impl->control_port);

	pw_log_info("Negotiated UDP streaming: data_port=%u control_port=%u",
			impl->data_port, impl->control_port);

	if ((impl->server_fd = connect_socket(impl, SOCK_DGRAM, -1, impl->data_port)) < 0) {
		pw_log_error("Could not connect to data port %u", impl->data_port);
		plist_free(response);
		return impl->server_fd;
	}

	if (impl->control_port != 0 && impl->control_fd >= 0) {
		int fd = connect_socket(impl, SOCK_DGRAM, impl->control_fd, impl->control_port);
		if (fd < 0) {
			/* connect_socket closed the old fd on failure */
			pw_log_warn("Could not connect to control port %u", impl->control_port);
			impl->control_fd = -1;
		} else {
			impl->control_fd = fd;
		}
	}

	if (impl->control_fd >= 0) {
		impl->control_source = pw_loop_add_io(impl->loop, impl->control_fd,
					SPA_IO_IN, false, on_control_source_io, impl);
		if (impl->control_source == NULL)
			pw_log_warn("Could not add control source: %m");
	}

	impl->ready = true;

	plist_free(response);

	if (rtp_stream_get_state(impl->stream, NULL) == PW_STREAM_STATE_STREAMING)
		return rtsp_do_ap2_record(impl);

	return 0;
}

static int rtsp_do_ap2_setup_stream(struct impl *impl)
{
	plist_t root;
	plist_t streams;
	plist_t stream;
	uint8_t *plist_bin = NULL;
	size_t plist_len = 0;
	const char *url;
	int res;

	stream = plist_new_dict();
	wplist_dict_add_uint(stream, "audioFormat", 0x40000);
	wplist_dict_add_string(stream, "audioMode", "default");
	wplist_dict_add_uint(stream, "controlPort", impl->control_port);
	wplist_dict_add_uint(stream, "ct", 2);
	wplist_dict_add_bool(stream, "isMedia", true);
	wplist_dict_add_uint(stream, "latencyMax", 88200);
	wplist_dict_add_uint(stream, "latencyMin", RAOP_LATENCY_MIN);
	wplist_dict_add_data(stream, "shk", impl->shared_secret, AP_AUDIO_KEY_LEN);
	wplist_dict_add_uint(stream, "spf", FRAMES_PER_UDP_PACKET);
	wplist_dict_add_uint(stream, "sr", impl->rate);
	wplist_dict_add_uint(stream, "type", 0x60);
	wplist_dict_add_bool(stream, "supportsDynamicStreamID", false);
	wplist_dict_add_uint(stream, "streamConnectionID", strtoull(impl->session_id, NULL, 10));

	streams = plist_new_array();
	plist_array_append_item(streams, stream);

	root = plist_new_dict();
	plist_dict_set_item(root, "streams", streams);

	if (wplist_to_bin(&plist_bin, &plist_len, root) < 0) {
		pw_log_error("Failed to serialize stream SETUP plist");
		plist_free(root);
		return -1;
	}

	url = pw_rtsp_client_get_url(impl->rtsp);

	res = pw_rtsp_client_url_send(impl->rtsp, url, "SETUP", &impl->headers->dict,
				"application/x-apple-binary-plist", plist_bin, plist_len,
				rtsp_ap2_setup_stream_reply, impl);

	plist_free(root);
	free(plist_bin);

	return res;
}

static int rtsp_ap2_setup_session_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	plist_t response;
	uint16_t remote_timing_port = 0;
	int ret;

	pw_log_info("AP2 setup session status: %d", status);
	switch (status) {
	case 200:
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}

	ret = wplist_from_bin(&response, content);
	if (ret < 0) {
		pw_log_error("Could not parse plist from session SETUP response");
		return -1;
	}

	/* eventPort is the receiver's; store it (it stays 0 if absent/malformed).
	 * timingPort is also the receiver's, but impl->timing_port already holds
	 * OUR advertised local timing port (bound for the SETUP request) — do not
	 * clobber it. Capture the receiver's value for the log only; nothing
	 * downstream consumes it yet. */
	plist_dict_get_port(response, "eventPort", &impl->events_port);
	plist_dict_get_port(response, "timingPort", &remote_timing_port);

	pw_log_info("Session SETUP response: eventPort=%u timingPort=%u",
			impl->events_port, remote_timing_port);

	plist_free(response);

	return rtsp_do_ap2_setpeers(impl);
}

// Executes SHA512 RFC 5869 extract + expand, writing a derived key to okm
static int
hkdf_extract_expand(uint8_t *okm, size_t okm_len, const uint8_t *ikm, size_t ikm_len, const char *salt, const char *info)
{
	EVP_PKEY_CTX *pctx;

	if (okm_len > SHA512_DIGEST_LENGTH)
		return -1;
	if (! (pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL)))
		return -1;
	if (EVP_PKEY_derive_init(pctx) <= 0)
		goto error;
	if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha512()) <= 0)
		goto error;
	if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, (const unsigned char *)salt, strlen(salt)) <= 0)
		goto error;
	if (EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm, ikm_len) <= 0)
		goto error;
	if (EVP_PKEY_CTX_add1_hkdf_info(pctx, (const unsigned char *)info, strlen(info)) <= 0)
		goto error;
	if (EVP_PKEY_derive(pctx, okm, &okm_len) <= 0)
		goto error;
	EVP_PKEY_CTX_free(pctx);	
	return 0;
 error:
	EVP_PKEY_CTX_free(pctx);
	return -1;
}

static struct pw_rtsp_cipher_context *ap_cipher_context_new(const uint8_t *shared_secret, size_t shared_secret_len, const char *enc_salt, const char *enc_info, const char *dec_salt, const char *dec_info)
{
	struct pw_rtsp_cipher_context *cctx;
	int ret;

	cctx = calloc(1, sizeof(*cctx));
	if (!cctx)
		goto error;
	ret = hkdf_extract_expand(cctx->encryption_key, sizeof(cctx->encryption_key), shared_secret, shared_secret_len, enc_salt, enc_info);
	if (ret < 0)
		goto error;
	ret = hkdf_extract_expand(cctx->decryption_key, sizeof(cctx->decryption_key), shared_secret, shared_secret_len, dec_salt, dec_info);
	if (ret < 0)
		goto error;
	return cctx;
error:
	free(cctx);
	return NULL;
}

static ssize_t ap_encrypt(uint8_t **ciphertext, size_t *ciphertext_len,
		const uint8_t *plaintext, size_t plaintext_len,
		struct pw_rtsp_cipher_context *cctx);
static ssize_t ap_decrypt(uint8_t **plaintext, size_t *plaintext_len,
		const uint8_t *ciphertext, size_t ciphertext_len,
		struct pw_rtsp_cipher_context *cctx);

/* Derive the control-channel cipher from the shared secret and install it on
 * the RTSP client. Replaces any existing context. This is the single place
 * that enables channel encryption, so creating and installing the cipher can
 * never get out of step. */
static int install_control_cipher(struct impl *impl,
		const char *enc_salt, const char *enc_info,
		const char *dec_salt, const char *dec_info)
{
	struct pw_rtsp_cipher_context *cctx;

	cctx = ap_cipher_context_new(impl->shared_secret, impl->shared_secret_len,
			enc_salt, enc_info, dec_salt, dec_info);
	if (cctx == NULL) {
		pw_log_error("Failed to derive control-channel cipher");
		return -EIO;
	}

	free(impl->cipher_ctx);
	impl->cipher_ctx = cctx;
	pw_rtsp_client_set_cipher_ctx(impl->rtsp, impl->cipher_ctx);
	pw_rtsp_client_set_encryption(impl->rtsp, ap_encrypt);
	pw_rtsp_client_set_decryption(impl->rtsp, ap_decrypt);
	return 0;
}

/* AirPlay 2 control-channel framing is little-endian on the wire (block-length
 * prefix and the 8-byte nonce counter), independent of host byte order. */
static inline void le16_store(uint8_t p[2], uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
static inline uint16_t le16_load(const uint8_t p[2])
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void le64_store(uint8_t p[8], uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static int chacha20_poly1305_encrypt(uint8_t *cipher, const uint8_t *plain, size_t plain_len,
					const uint8_t *key, size_t key_len, const void *ad, size_t ad_len,
					uint8_t *tag, size_t tag_len, const uint8_t nonce[AP_NONCE_LENGTH]) {
	EVP_CIPHER_CTX *ctx;
	int len;

	if (!(ctx = EVP_CIPHER_CTX_new()))
		return -1;
	if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, key, nonce) != 1)
		goto error;
	if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) // Maybe not necessary
		goto error;
	if (ad_len > 0 && EVP_EncryptUpdate(ctx, NULL, &len, ad, ad_len) != 1)
		goto error;
	if (EVP_EncryptUpdate(ctx, cipher, &len, plain, plain_len) != 1)
		goto error;
	assert((size_t)len == plain_len);
	if (EVP_EncryptFinal_ex(ctx, NULL, &len) != 1)
		goto error;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, tag_len, tag) != 1)
		goto error;
	EVP_CIPHER_CTX_free(ctx);
	return 0;
error:
	EVP_CIPHER_CTX_free(ctx);
	return -1;
}

static ssize_t ap_encrypt(uint8_t **ciphertext, size_t *ciphertext_len,
						const uint8_t *plaintext, size_t plaintext_len,
						struct pw_rtsp_cipher_context *cctx)
{
	uint8_t nonce[AP_NONCE_LENGTH] = { 0 };
	uint8_t tag[AP_AUTHTAG_LENGTH];
	uint8_t len_le[sizeof(uint16_t)];
	const uint8_t *plain_block;
	uint8_t *cipher_block;
	uint16_t block_len;
	int nblocks;
	int ret;
	int i;

	if (plaintext_len == 0 || !plaintext)
		return -1;

	// Encryption is done in blocks, where each block consists of a short, the
	// encrypted data and an auth tag. The short is the size of the encrypted
	// data. The encrypted data in the block cannot exceed AP_ENCRYPTED_BLOCK_LENGTH_MAX == 1024.
	nblocks = 1 + ((plaintext_len - 1) / AP_ENCRYPTED_BLOCK_LENGTH_MAX); // Ceiling of division

	*ciphertext_len = nblocks * (sizeof(block_len) + AP_AUTHTAG_LENGTH) + plaintext_len;
	if ((*ciphertext = malloc(*ciphertext_len)) == NULL)
		return -1;

	cctx->encryption_counter_prev = cctx->encryption_counter;

	for (i = 0, plain_block = plaintext, cipher_block = *ciphertext; i < nblocks; i++) {
		// If it is the last block we will encrypt only the remaining data
		block_len = (i + 1 == nblocks) ? (plaintext + plaintext_len - plain_block) : AP_ENCRYPTED_BLOCK_LENGTH_MAX;

		le64_store(nonce + 4, cctx->encryption_counter);
		le16_store(len_le, block_len);

		// Write the ciphered block: the little-endian length prefix is both
		// the wire framing and the AEAD associated data.
		memcpy(cipher_block, len_le, sizeof(len_le));
		ret = chacha20_poly1305_encrypt(cipher_block + sizeof(block_len),
				plain_block, block_len, cctx->encryption_key, sizeof(cctx->encryption_key),
				len_le, sizeof(len_le), tag, sizeof(tag), nonce);
		if (ret < 0) {
	  		pw_log_error("Encryption with chacha poly1305 failed");
	  		cctx->encryption_counter = cctx->encryption_counter_prev;
	  		free(*ciphertext);
	  		*ciphertext = NULL;
	  		return -1;
		}
		memcpy(cipher_block + sizeof(block_len) + block_len, tag, AP_AUTHTAG_LENGTH);

		plain_block += block_len;
		cipher_block += block_len + sizeof(block_len) + AP_AUTHTAG_LENGTH;
		cctx->encryption_counter++;
	}

	return plain_block - plaintext;
}


// Hex string to bytes conversion
static void hexread(uint8_t *out, size_t out_len, const char *in)
{
	char hex[3] = { 0 };
	size_t i;

	for (i = 0; i < out_len && in[0] && in[1]; i++, in += 2) {
		hex[0] = in[0];
		hex[1] = in[1];
		out[i] = strtol(hex, NULL, 16);
	}
}

// Bytes to lowercase hex string. out must hold 2*in_len + 1 bytes.
static void bytes_to_hex(char *out, const uint8_t *in, size_t in_len)
{
	static const char h[] = "0123456789abcdef";
	size_t i;
	for (i = 0; i < in_len; i++) {
		out[i * 2]     = h[in[i] >> 4];
		out[i * 2 + 1] = h[in[i] & 0x0f];
	}
	out[in_len * 2] = '\0';
}

// Generate X25519 key pair
static int x25519_keypair(uint8_t *public_key, uint8_t *private_key)
{
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *pctx = NULL;
	size_t key_len = AP_X25519_KEY_LENGTH;
	int ret = -1;

	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
	if (!pctx)
		goto error;

	if (EVP_PKEY_keygen_init(pctx) <= 0)
		goto error;

	if (EVP_PKEY_keygen(pctx, &pkey) <= 0)
		goto error;

	if (EVP_PKEY_get_raw_public_key(pkey, public_key, &key_len) <= 0)
		goto error;

	key_len = AP_X25519_KEY_LENGTH;
	if (EVP_PKEY_get_raw_private_key(pkey, private_key, &key_len) <= 0)
		goto error;

	ret = 0;
error:
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(pctx);
	return ret;
}

// Generate an Ed25519 long-term key pair. private_key holds the 32-byte seed
// followed by the 32-byte public key (the layout parse_auth_key/ed25519_sign use).
static int ed25519_keypair(uint8_t *public_key, uint8_t *private_key)
{
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *pctx = NULL;
	size_t len;
	int ret = -1;

	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
	if (!pctx)
		goto error;
	if (EVP_PKEY_keygen_init(pctx) <= 0)
		goto error;
	if (EVP_PKEY_keygen(pctx, &pkey) <= 0)
		goto error;

	len = AP_ED25519_PUBLIC_KEY_LENGTH;
	if (EVP_PKEY_get_raw_public_key(pkey, public_key, &len) <= 0)
		goto error;

	len = AP_ED25519_SEED_LENGTH;
	if (EVP_PKEY_get_raw_private_key(pkey, private_key, &len) <= 0)
		goto error;
	memcpy(private_key + AP_ED25519_SEED_LENGTH, public_key, AP_ED25519_PUBLIC_KEY_LENGTH);

	ret = 0;
error:
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(pctx);
	return ret;
}

// X25519 key exchange: derive shared secret
static int x25519_shared_secret(uint8_t *shared_secret, const uint8_t *private_key, const uint8_t *peer_public_key)
{
	EVP_PKEY *pkey = NULL;
	EVP_PKEY *peer_key = NULL;
	EVP_PKEY_CTX *pctx = NULL;
	size_t secret_len = AP_X25519_KEY_LENGTH;
	int ret = -1;

	pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, AP_X25519_KEY_LENGTH);
	if (!pkey)
		goto error;

	peer_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public_key, AP_X25519_KEY_LENGTH);
	if (!peer_key)
		goto error;

	pctx = EVP_PKEY_CTX_new(pkey, NULL);
	if (!pctx)
		goto error;

	if (EVP_PKEY_derive_init(pctx) <= 0)
		goto error;

	if (EVP_PKEY_derive_set_peer(pctx, peer_key) <= 0)
		goto error;

	if (EVP_PKEY_derive(pctx, shared_secret, &secret_len) <= 0)
		goto error;

	ret = 0;
error:
	EVP_PKEY_free(pkey);
	EVP_PKEY_free(peer_key);
	EVP_PKEY_CTX_free(pctx);
	return ret;
}

// Ed25519 sign
static int ed25519_sign(uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *private_key)
{
	EVP_PKEY *pkey = NULL;
	EVP_MD_CTX *mdctx = NULL;
	size_t sig_len = AP_ED25519_SIGNATURE_LENGTH;
	int ret = -1;

	/* OpenSSL expects the 32-byte seed, not the 64-byte seed||pubkey blob. */
	pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, AP_ED25519_SEED_LENGTH);
	if (!pkey)
		goto error;

	mdctx = EVP_MD_CTX_new();
	if (!mdctx)
		goto error;

	if (EVP_DigestSignInit(mdctx, NULL, NULL, NULL, pkey) <= 0)
		goto error;

	if (EVP_DigestSign(mdctx, signature, &sig_len, message, message_len) <= 0)
		goto error;

	ret = 0;
error:
	EVP_PKEY_free(pkey);
	EVP_MD_CTX_free(mdctx);
	return ret;
}

// Ed25519 verify
static int ed25519_verify(const uint8_t *signature, const uint8_t *message, size_t message_len, const uint8_t *public_key)
{
	EVP_PKEY *pkey = NULL;
	EVP_MD_CTX *mdctx = NULL;
	int ret = -1;

	pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, AP_ED25519_PUBLIC_KEY_LENGTH);
	if (!pkey)
		goto error;

	mdctx = EVP_MD_CTX_new();
	if (!mdctx)
		goto error;

	if (EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey) <= 0)
		goto error;

	if (EVP_DigestVerify(mdctx, signature, AP_ED25519_SIGNATURE_LENGTH, message, message_len) != 1)
		goto error;

	ret = 0;
error:
	EVP_PKEY_free(pkey);
	EVP_MD_CTX_free(mdctx);
	return ret;
}

// HKDF extract + expand for pair-verify (using salt and info strings)
static int chacha20_poly1305_decrypt(uint8_t *plain, const uint8_t *cipher, size_t cipher_len,
					const uint8_t *key, size_t key_len, const void *ad, size_t ad_len,
					uint8_t *tag, size_t tag_len, const uint8_t nonce[AP_NONCE_LENGTH])
{
	EVP_CIPHER_CTX *ctx;
	int len;

	if (! (ctx = EVP_CIPHER_CTX_new()))
		return -1;
	if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, key, nonce) != 1)
		goto error;
	if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) // Maybe not necessary
		goto error;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, tag_len, tag) != 1)
		goto error;
	if (ad_len > 0 && EVP_DecryptUpdate(ctx, NULL, &len, ad, ad_len) != 1)
		goto error;
	if (EVP_DecryptUpdate(ctx, plain, &len, cipher, cipher_len) != 1)
		goto error;
	if (EVP_DecryptFinal_ex(ctx, NULL, &len) != 1)
		goto error;
	EVP_CIPHER_CTX_free(ctx);
	return 0;
error:
	EVP_CIPHER_CTX_free(ctx);
	return -1;
}

static ssize_t ap_decrypt(uint8_t **plaintext, size_t *plaintext_len,
						const uint8_t *ciphertext, size_t ciphertext_len,
						struct pw_rtsp_cipher_context *cctx)
{
	uint8_t nonce[AP_NONCE_LENGTH] = { 0 };
	uint8_t tag[AP_AUTHTAG_LENGTH];
	uint8_t *plain_block;
	const uint8_t *cipher_block;
	uint16_t block_len;
	int ret;

	*plaintext = NULL;
	*plaintext_len = 0;

	// Not enough bytes for even a length prefix yet: ask the caller for more.
	if (!ciphertext || ciphertext_len < sizeof(block_len))
		return 0;

	// This will allocate more than we need. Since we don't know the number of
	// blocks in the ciphertext yet we can't calculate the exact required length.
	if ((*plaintext = malloc(ciphertext_len)) == NULL)
		return -1;

	cctx->decryption_counter_prev = cctx->decryption_counter;

	// Only enter the loop while a full length prefix is readable.
	for (plain_block = *plaintext, cipher_block = ciphertext;
	     (size_t)(ciphertext + ciphertext_len - cipher_block) >= sizeof(block_len); ) {
		uint8_t len_le[sizeof(uint16_t)];

		memcpy(len_le, cipher_block, sizeof(len_le));
		block_len = le16_load(len_le);
		if ((size_t)(ciphertext + ciphertext_len - cipher_block) <
				(size_t)block_len + sizeof(block_len) + AP_AUTHTAG_LENGTH) {
			// The remaining ciphertext doesn't contain an entire block, so stop
			break;
		}

		memcpy(tag, cipher_block + sizeof(block_len) + block_len, sizeof(tag));
		le64_store(nonce + 4, cctx->decryption_counter);

		ret = chacha20_poly1305_decrypt(plain_block, cipher_block + sizeof(block_len), block_len, cctx->decryption_key, sizeof(cctx->decryption_key), len_le, sizeof(len_le), tag, sizeof(tag), nonce);
		if (ret < 0) {
			pw_log_error("Decryption with chacha poly1305 failed");
			cctx->decryption_counter = cctx->decryption_counter_prev;
			free(*plaintext);
			*plaintext = NULL;
			return -1;
		}

		plain_block += block_len;
		cipher_block += block_len + sizeof(block_len) + AP_AUTHTAG_LENGTH;
		cctx->decryption_counter++;
	}

	*plaintext_len = plain_block - *plaintext;

	return cipher_block - ciphertext;
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
		if (impl->timing_source == NULL)
			goto error;

		switch (impl->encryption) {
		case CRYPTO_PAIR_TRANSIENT:
		case CRYPTO_PAIR_HOMEKIT:
		case CRYPTO_PAIR_APPLE: {
			/* AirPlay 2 Session SETUP
			 * First SETUP request establishes the session with
			 * deviceID, sessionUUID, timingPort, timingProtocol */
			plist_t root_plist;
			uint8_t *plist_bin = NULL;
			size_t plist_len = 0;
			char device_id[24];
			int ip_version;
			char local_ip[INET6_ADDRSTRLEN];

			pw_rtsp_client_get_local_ip(impl->rtsp, &ip_version, local_ip, sizeof(local_ip));

			/* Generate session UUID if not already set */
			if (impl->session_uuid[0] == '\0') {
				uint8_t uuid_bytes[16];
				if (pw_getrandom(uuid_bytes, sizeof(uuid_bytes), 0) < 0) {
					pw_log_error("Failed to generate session UUID");
					goto error;
				}
				snprintf(impl->session_uuid, sizeof(impl->session_uuid),
					"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
					uuid_bytes[0], uuid_bytes[1], uuid_bytes[2], uuid_bytes[3],
					uuid_bytes[4], uuid_bytes[5], uuid_bytes[6], uuid_bytes[7],
					uuid_bytes[8], uuid_bytes[9], uuid_bytes[10], uuid_bytes[11],
					uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);
			}

			/* Create device ID from local IP (AA:BB:CC:DD:EE:FF format) */
			uint8_t mac_bytes[6];
			if (pw_getrandom(mac_bytes, sizeof(mac_bytes), 0) < 0) {
				pw_log_error("Failed to generate device ID");
				goto error;
			}
			snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
				mac_bytes[0], mac_bytes[1], mac_bytes[2],
				mac_bytes[3], mac_bytes[4], mac_bytes[5]);

			root_plist = plist_new_dict();
			wplist_dict_add_string(root_plist, "deviceID", device_id);
			wplist_dict_add_string(root_plist, "sessionUUID", impl->session_uuid);
			wplist_dict_add_uint(root_plist, "timingPort", impl->timing_port);
			wplist_dict_add_string(root_plist, "timingProtocol", "NTP");

			if (wplist_to_bin(&plist_bin, &plist_len, root_plist) < 0) {
				pw_log_error("Failed to serialize session SETUP plist");
				plist_free(root_plist);
				goto error;
			}

			const char *url = pw_rtsp_client_get_url(impl->rtsp);

			/* Enable the encrypted control channel if pairing has not
			 * already installed it. */
			if (!impl->cipher_ctx) {
				if (install_control_cipher(impl, AP_CONTROL_SALT, AP_CONTROL_ENC_INFO,
							AP_CONTROL_SALT, AP_CONTROL_DEC_INFO) < 0) {
					plist_free(root_plist);
					free(plist_bin);
					goto error;
				}
			}

			res = pw_rtsp_client_url_send(impl->rtsp, url, "SETUP", &impl->headers->dict,
						"application/x-apple-binary-plist", plist_bin, plist_len,
						rtsp_ap2_setup_session_reply, impl);

			plist_free(root_plist);
			free(plist_bin);

			return res;
		}
		default:
			pw_properties_setf(impl->headers, "Transport",
					"RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
					"control_port=%u;timing_port=%u",
					impl->control_port, impl->timing_port);
		}

		break;

	default:
		return -ENOTSUP;
	}

	res = rtsp_send(impl, "SETUP", NULL, NULL, rtsp_raop_setup_reply);

	pw_properties_set(impl->headers, "Transport", NULL);

	return res;
error:
	/* Destroy the timing source (if already created) BEFORE closing its fd, so
	 * the loop performs EPOLL_CTL_DEL while the fd is still valid and we do not
	 * leak the source or leave it bound to a recycled fd. Mirrors
	 * connection_cleanup's ordering. */
	if (impl->timing_source != NULL) {
		pw_loop_destroy_source(impl->loop, impl->timing_source);
		impl->timing_source = NULL;
	}
	if (impl->control_fd >= 0) {
		close(impl->control_fd);
		impl->control_fd = -1;
	}
	if (impl->timing_fd >= 0) {
		close(impl->timing_fd);
		impl->timing_fd = -1;
	}
	return -EIO;
}

static int rtsp_raop_announce_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;

	pw_log_info("announce status: %d", status);
	switch (status) {
	case 200:
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}

	pw_properties_set(impl->headers, "Apple-Challenge", NULL);

	return rtsp_do_setup(impl);
}

static inline void swap_bytes(uint8_t *data, size_t size)
{
	int i, j;
	for (i = 0, j = size-1; i < j; i++, j--)
		SPA_SWAP(data[i], data[j]);
}

static int rsa_encrypt(uint8_t *data, int len, uint8_t *enc, size_t enc_max)
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

	msize = pw_base64_decode(n, strlen(n), modulus);
	esize = pw_base64_decode(e, strlen(e), exponent);

#if OPENSSL_API_LEVEL >= 30000
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	OSSL_PARAM params[5];
	int err = 0;
	size_t size = enc_max;

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

static int rtsp_do_raop_announce(struct impl *impl)
{
	const char *host;
	uint8_t rsa_key[512];
	char rsa_key_b64enc[512*2];
	char rsa_iv_b64enc[16*2];
	uint32_t rtp_latency;
	int res, rsa_len, ip_version;
	spa_autofree char *sdp = NULL;
	char local_ip[256];
	host = pw_properties_get(impl->props, "raop.ip");
	rtp_latency = msec_to_samples(impl, RAOP_LATENCY_MS);

	pw_rtsp_client_get_local_ip(impl->rtsp, &ip_version,
			local_ip, sizeof(local_ip));

	switch (impl->encryption) {
	case CRYPTO_NONE:
	case CRYPTO_FP_SAP25:
		sdp = spa_aprintf("v=0\r\n"
				"o=iTunes %s 0 IN IP%d %s\r\n"
				"s=iTunes\r\n"
				"c=IN IP%d %s\r\n"
				"t=0 0\r\n"
				"m=audio 0 RTP/AVP 96\r\n"
				"a=rtpmap:96 AppleLossless\r\n"
				"a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 %u\r\n",
				impl->session_id, ip_version, local_ip,
				ip_version, host, impl->psamples, impl->rate);
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
				ip_version, host, impl->psamples, impl->rate,
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

		pw_base64_encode(rac, sizeof(rac), sac, '\0');
		pw_properties_set(impl->headers, "Apple-Challenge", sac);

		rsa_len = rsa_encrypt(impl->aes_key, 16, rsa_key, sizeof(rsa_key));
		if (rsa_len < 0)
			return -rsa_len;

		pw_base64_encode(rsa_key, rsa_len, rsa_key_b64enc, '=');
		pw_base64_encode(impl->aes_iv, 16, rsa_iv_b64enc, '=');

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
				ip_version, host, impl->psamples, impl->rate,
				rsa_key_b64enc, rsa_iv_b64enc);
		if (!sdp)
			return -errno;
		break;
	}
	default:
		return -ENOTSUP;
	}

	return rtsp_send(impl, "ANNOUNCE", "application/sdp", sdp, rtsp_raop_announce_reply);
}

static int rtsp_do_raop_auth_setup(struct impl *impl);
static int rtsp_parse_auth(struct impl *impl, const struct spa_dict *headers);
static void rtsp_clear_auth(struct impl *impl);

static int rtsp_raop_auth_setup_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res;

	pw_log_info("auth-setup status: %d", status);
	switch (status) {
	case 200:
		break;
	case 401:
		if (impl->auth_setup_auth_retry) {
			pw_impl_module_schedule_destroy(impl->module);
			return 0;
		}
		if ((res = rtsp_parse_auth(impl, headers)) < 0) {
			pw_impl_module_schedule_destroy(impl->module);
			return 0;
		}
		impl->auth_setup_auth_retry = true;
		return rtsp_do_raop_auth_setup(impl);
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}

	return rtsp_do_raop_announce(impl);
}

static int rtsp_do_raop_auth_setup(struct impl *impl)
{
	static const uint8_t content[33] = {
		0x01,
		0x59, 0x02, 0xed, 0xe9, 0x0d, 0x4e, 0xf2, 0xbd,
		0x4c, 0xb6, 0x8a, 0x63, 0x30, 0x03, 0x82, 0x07,
		0xa9, 0x4d, 0xbd, 0x50, 0xd8, 0xaa, 0x46, 0x5b,
		0x5d, 0x8c, 0x01, 0x2a, 0x0c, 0x7e, 0x1d, 0x4e };

	int res;

	res = rtsp_send_url(impl, "/auth-setup", "POST", "application/octet-stream",
			content, sizeof(content), rtsp_raop_auth_setup_reply);
	if (res < 0)
		pw_impl_module_schedule_destroy(impl->module);
	return res;
}

static int rtsp_raop_auth_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res = 0;

	pw_log_info("raop auth status: %d", status);

	switch (status) {
	case 200:
		if (impl->encryption == CRYPTO_AUTH_SETUP)
			res = rtsp_do_raop_auth_setup(impl);
		else
			res = rtsp_do_raop_announce(impl);
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
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

static void rtsp_clear_auth(struct impl *impl)
{
	free(impl->auth_method);
	impl->auth_method = NULL;
	if (impl->realm) {
		explicit_bzero(impl->realm, strlen(impl->realm));
		free(impl->realm);
		impl->realm = NULL;
	}
	if (impl->nonce) {
		explicit_bzero(impl->nonce, strlen(impl->nonce));
		free(impl->nonce);
		impl->nonce = NULL;
	}
}

static int rtsp_parse_auth(struct impl *impl, const struct spa_dict *headers)
{
	const char *str, *realm, *nonce;
	char *auth_method = NULL, *realm_copy = NULL, *nonce_copy = NULL;
	int n_tokens;
	int res = 0;

	if ((str = spa_dict_lookup(headers, "WWW-Authenticate")) == NULL)
		return -EINVAL;

	if (impl->password == NULL) {
		pw_log_warn("authentication required but no raop.password property was given");
		return -ENOTSUP;
	}

	pw_log_debug("Auth: %s", str);

	spa_auto(pw_strv) tokens = pw_split_strv(str, " ", INT_MAX, &n_tokens);
	if (tokens == NULL || tokens[0] == NULL)
		return -EINVAL;

	auth_method = strdup(tokens[0]);
	if (auth_method == NULL)
		return -ENOMEM;

	if (spa_streq(auth_method, "Digest")) {
		realm = find_attr(tokens, "realm");
		nonce = find_attr(tokens, "nonce");
		if (realm == NULL || nonce == NULL)
			goto error;

		realm_copy = strdup(realm);
		nonce_copy = strdup(nonce);
		if (realm_copy == NULL || nonce_copy == NULL) {
			res = -ENOMEM;
			goto error;
		}
	}

	rtsp_clear_auth(impl);
	impl->auth_method = auth_method;
	impl->realm = realm_copy;
	impl->nonce = nonce_copy;
	return 0;

error:
	if (res == 0)
		res = -EINVAL;
	free(auth_method);
	if (realm_copy) {
		explicit_bzero(realm_copy, strlen(realm_copy));
		free(realm_copy);
	}
	if (nonce_copy) {
		explicit_bzero(nonce_copy, strlen(nonce_copy));
		free(nonce_copy);
	}
	return res;
}

static int rtsp_do_raop_auth(struct impl *impl, const struct spa_dict *headers)
{
	int res;

	res = rtsp_parse_auth(impl, headers);
	if (res >= 0)
		res = rtsp_send(impl, "OPTIONS", NULL, NULL, rtsp_raop_auth_reply);
	if (res < 0)
		pw_impl_module_schedule_destroy(impl->module);
	return res;
}

static tlv_values_t * tlv_message_process(const uint8_t *data, size_t data_len)
{
	tlv_values_t *response;
	tlv_t *error;
	int ret;

	pw_log_debug("tlv: processing tlv message");

	response = tlv_new();
	if (!response) {
		pw_log_error("tlv: Out of memory");
		return NULL;
	}

	ret = tlv_parse(data, data_len, response);
	if (ret < 0) {
		pw_log_error("tlv: Could not parse TLV");
		goto error;
	}
	
  	for (tlv_t *t=response->head; t; t=t->next) {
		pw_log_debug("tlv: parsed type %d value (%zu bytes)", t->type, t->size);
	}

	error = tlv_get_value(response, AP2_TLVType_Error);
	if (error) {
		/* A zero-length Error TLV parses with value == NULL; never index it. */
		uint8_t code = (error->size >= 1) ? error->value[0] : 0;
		if (code == AP2_TLVError_Authentication)
			pw_log_error("tlv: Device returned an authentication failure");
		else if (code == AP2_TLVError_Backoff)
			pw_log_error("tlv: Device told us to back off pairing attempts");
		else if (code == AP2_TLVError_MaxPeers)
			pw_log_error("tlv: Max peers trying to connect to device");
	  	else if (code == AP2_TLVError_MaxTries)
			pw_log_error("tlv: Max pairing attempts reached");
	  	else if (code == AP2_TLVError_Unavailable)
			pw_log_error("tlv: Device is unavailable at this time");
		else
			pw_log_error("tlv: Device is busy/returned unknown error (%u)", code);

		goto error;
	}

  return response;

error:
	tlv_free(response);
	return NULL;
}

static int rtsp_ap2_pair_setup2_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	tlv_values_t *response;
	tlv_t *proof;
	const uint8_t *session_key;
	int session_key_len;
	uint32_t content_length;
	const char *str;

	pw_log_info("pair-setup (req 2) status: %d", status);

	if (status != 200) {
		pw_log_error("pair-setup (req 2): Server returned error %d", status);
		return -1;
	}

	if ((str = spa_dict_lookup(headers, "Content-Length")) == NULL) {
		pw_log_error("pair-setup (req 2) res: Missing Content-Length");
		return -1;
	}
	
	if (!spa_atou32(str, &content_length, 0)) {
		pw_log_error("pair-setup (req 2) res: Could not read Content-Length");
		return -1;
	}
	pw_log_debug("pair-setup (req 2) res: Content-Length: %s", str);

	/* Bound the parse by the actually-buffered body: a peer-declared
	 * Content-Length larger than what we received would read past content->data. */
	response = tlv_message_process((const uint8_t *)content->data,
			SPA_MIN((size_t) content_length, content->size));
	if (!response) {
		pw_log_error("pair-setup (req 2) res: Received an error");
		return -1;
	}

	proof = tlv_get_value(response, AP2_TLVType_Proof);
	if (!proof || proof->size != SHA512_DIGEST_LENGTH) {
		pw_log_error("pair-setup (req 2) res: Missing or invalid proof");
		goto error;
	}

	impl->M2_len = proof->size;
  	impl->M2 = malloc(impl->M2_len);
	if (impl->M2 == NULL) {
		pw_log_error("pair-setup (req 2) res: out of memory");
		goto error;
	}
  	memcpy(impl->M2, proof->value, impl->M2_len);

	// Check M2
	srp_user_verify_session(impl->srp_user, (const unsigned char *)impl->M2);
	if (!srp_user_is_authenticated(impl->srp_user)) {
		pw_log_error("pair-setup (req 2) res: Server authentication failed");
		goto error;
	}

	session_key = srp_user_get_session_key(impl->srp_user, &session_key_len);
	if (!session_key) {
		pw_log_error("pair-setup (req 2) res: Could not compute session key");
		goto error;
	}

	if (sizeof(impl->shared_secret) < (unsigned long int) session_key_len) {
		pw_log_error("pair-setup (req 2) res: invalid session key");
		goto error;
	}

	memcpy(impl->shared_secret, session_key, session_key_len);
	impl->shared_secret_len = session_key_len;

	tlv_free(response);

	/* Full (HomeKit/Apple) pairing: continue with M5/M6 to exchange long-term
	 * keys and obtain a persistent authorization key. Transient pairing stops
	 * here and derives the ephemeral control cipher directly. */
	if (impl->pairing_pin)
		return rtsp_do_ap2_pair_setup3(impl);

	// After successful transient pairing, derive and install the control-channel
	// encryption keys (HKDF-SHA-512) so the SETUP request goes out encrypted.
	if (impl->shared_secret_len > 0) {
		if (install_control_cipher(impl, AP_CONTROL_SALT, AP_CONTROL_ENC_INFO,
					AP_CONTROL_SALT, AP_CONTROL_DEC_INFO) < 0)
			return -1;
		pw_log_info("Derived encryption keys for both directions after pairing");
	}

	pw_log_info("received shared secret");

	pw_properties_set(impl->headers, "X-Apple-HKP", NULL);

	return rtsp_do_setup(impl);

error:
	tlv_free(response);
	return -1;
}

/* Persist the freshly-minted authorization key: log it (so it can be added to
 * raop.auth_key) and, if raop.auth-key-file is configured, write it there. */
static void pairing_store_auth_key(struct impl *impl)
{
	char hex[2 * (AP_ED25519_PRIVATE_KEY_LENGTH + AP_ED25519_PUBLIC_KEY_LENGTH) + 1];
	const char *path;

	bytes_to_hex(hex, impl->client_private_key, AP_ED25519_PRIVATE_KEY_LENGTH);
	bytes_to_hex(hex + 2 * AP_ED25519_PRIVATE_KEY_LENGTH,
			impl->server_public_key, AP_ED25519_PUBLIC_KEY_LENGTH);

	free(impl->auth_key);
	impl->auth_key = strdup(hex);

	pw_log_info("AirPlay pairing complete. Set raop.auth_key to this value "
			"(and remove raop.pairing-pin) to connect from now on:\n  %s", hex);

	if ((path = pw_properties_get(impl->props, "raop.auth-key-file")) != NULL) {
		FILE *f = fopen(path, "we");
		if (f == NULL) {
			pw_log_warn("Could not write auth key to '%s': %m", path);
		} else {
			fprintf(f, "%s\n", hex);
			fclose(f);
			pw_log_info("Wrote authorization key to '%s'", path);
		}
	}
}

/* Pair-setup M6: decrypt the accessory's long-term public key, verify its
 * signature, and store the resulting authorization key. */
static int rtsp_ap2_pair_setup3_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	tlv_values_t *response, *inner = NULL;
	tlv_t *err, *encrypted, *acc_id, *acc_pk, *acc_sig;
	uint8_t session_key[32], sign_key[32];
	uint8_t accessory_info[256];
	uint8_t nonce[AP_NONCE_LENGTH] = { 0 };
	uint8_t *decrypted = NULL;
	size_t encrypted_len, accessory_info_len;
	int ret;

	if (status != 200) {
		pw_log_error("pair-setup (req 3) status: %d", status);
		return -1;
	}

	response = tlv_message_process((const uint8_t *)content->data, content->size);
	if (!response) {
		pw_log_error("pair-setup (req 3) res: TLV parse error");
		return -1;
	}

	err = tlv_get_value(response, AP2_TLVType_Error);
	if (err && err->size >= 1 && err->value[0] != 0) {
		pw_log_error("pair-setup (req 3) res: accessory error %d (wrong PIN?)", err->value[0]);
		goto error;
	}

	encrypted = tlv_get_value(response, AP2_TLVType_EncryptedData);
	if (!encrypted || encrypted->size <= AP_AUTHTAG_LENGTH) {
		pw_log_error("pair-setup (req 3) res: missing encrypted data");
		goto error;
	}

	if (hkdf_extract_expand(session_key, sizeof(session_key), impl->shared_secret,
			impl->shared_secret_len, AP_PAIR_SETUP_ENC_SALT, AP_PAIR_SETUP_ENC_INFO) < 0) {
		pw_log_error("pair-setup (req 3) res: hkdf failed");
		goto error;
	}

	encrypted_len = encrypted->size - AP_AUTHTAG_LENGTH;
	decrypted = malloc(encrypted_len);
	if (decrypted == NULL)
		goto error;
	memcpy(nonce + 4, "PS-Msg06", 8);
	ret = chacha20_poly1305_decrypt(decrypted, encrypted->value, encrypted_len,
			session_key, sizeof(session_key), NULL, 0,
			encrypted->value + encrypted_len, AP_AUTHTAG_LENGTH, nonce);
	if (ret < 0) {
		pw_log_error("pair-setup (req 3) res: decryption failed");
		goto error;
	}

	inner = tlv_message_process(decrypted, encrypted_len);
	if (!inner) {
		pw_log_error("pair-setup (req 3) res: inner TLV parse error");
		goto error;
	}

	acc_id = tlv_get_value(inner, AP2_TLVType_Identifier);
	acc_pk = tlv_get_value(inner, AP2_TLVType_PublicKey);
	acc_sig = tlv_get_value(inner, AP2_TLVType_Signature);
	if (!acc_id || !acc_pk || !acc_sig ||
	    acc_pk->size != AP_ED25519_PUBLIC_KEY_LENGTH ||
	    acc_sig->size != AP_ED25519_SIGNATURE_LENGTH) {
		pw_log_error("pair-setup (req 3) res: missing/invalid accessory fields");
		goto error;
	}

	/* Verify the accessory's signature over AccessoryX || pairingID || LTPK. */
	if (hkdf_extract_expand(sign_key, sizeof(sign_key), impl->shared_secret,
			impl->shared_secret_len, AP_PAIR_SETUP_ACCESSORY_SIGN_SALT,
			AP_PAIR_SETUP_ACCESSORY_SIGN_INFO) < 0)
		goto error;

	accessory_info_len = sizeof(sign_key) + acc_id->size + acc_pk->size;
	if (accessory_info_len > sizeof(accessory_info)) {
		pw_log_error("pair-setup (req 3) res: accessory_info too small");
		goto error;
	}
	memcpy(accessory_info, sign_key, sizeof(sign_key));
	memcpy(accessory_info + sizeof(sign_key), acc_id->value, acc_id->size);
	memcpy(accessory_info + sizeof(sign_key) + acc_id->size, acc_pk->value, acc_pk->size);

	if (ed25519_verify(acc_sig->value, accessory_info, accessory_info_len, acc_pk->value) < 0) {
		pw_log_error("pair-setup (req 3) res: accessory signature verification failed");
		goto error;
	}

	memcpy(impl->server_public_key, acc_pk->value, AP_ED25519_PUBLIC_KEY_LENGTH);
	impl->have_server_public_key = true;

	pairing_store_auth_key(impl);

	free(decrypted);
	tlv_free(inner);
	tlv_free(response);
	pw_properties_set(impl->headers, "X-Apple-HKP", NULL);
	/* Pairing is a one-time provisioning step; the caller reconnects with
	 * raop.auth_key set (and raop.pairing-pin removed) to stream. */
	return 0;

error:
	free(decrypted);
	tlv_free(inner);
	tlv_free(response);
	return -1;
}

/* Pair-setup M5: send our long-term public key, identifier and a signature,
 * encrypted under the SRP-derived session key. */
static int rtsp_do_ap2_pair_setup3(struct impl *impl)
{
	tlv_values_t *request, *inner_tlv;
	uint8_t *data, *inner_data, *encrypted_data;
	size_t data_len = AP_REQUEST_BUFSIZE, inner_data_len, encrypted_data_len;
	size_t device_info_len, id_len = strlen(impl->device_id);
	uint8_t session_key[32], sign_key[32];
	uint8_t device_info[256];
	uint8_t signature[AP_ED25519_SIGNATURE_LENGTH];
	uint8_t nonce[AP_NONCE_LENGTH] = { 0 };
	uint8_t tag[AP_AUTHTAG_LENGTH];
	uint8_t state = 0x05;
	int ret;

	/* Generate the controller long-term Ed25519 key pair (persisted below). */
	if (ed25519_keypair(impl->client_public_key, impl->client_private_key) < 0) {
		pw_log_error("pair-setup 3: failed to generate long-term key");
		return -1;
	}

	/* Bind our pairing identifier to the just-minted identity so a later
	 * pair-verify (which re-derives it via parse_auth_key) presents the same
	 * Identifier the accessory is about to record. */
	derive_device_id(impl);

	/* SessionKey encrypts M5/M6; the controller sign key is folded into the
	 * signed device info. */
	if (hkdf_extract_expand(session_key, sizeof(session_key), impl->shared_secret,
			impl->shared_secret_len, AP_PAIR_SETUP_ENC_SALT, AP_PAIR_SETUP_ENC_INFO) < 0 ||
	    hkdf_extract_expand(sign_key, sizeof(sign_key), impl->shared_secret,
			impl->shared_secret_len, AP_PAIR_SETUP_CONTROLLER_SIGN_SALT,
			AP_PAIR_SETUP_CONTROLLER_SIGN_INFO) < 0) {
		pw_log_error("pair-setup 3: hkdf failed");
		return -1;
	}

	/* iOSDeviceInfo = iOSDeviceX || iOSDevicePairingID || iOSDeviceLTPK */
	device_info_len = sizeof(sign_key) + id_len + AP_ED25519_PUBLIC_KEY_LENGTH;
	if (device_info_len > sizeof(device_info)) {
		pw_log_error("pair-setup 3: device_info buffer too small");
		return -1;
	}
	memcpy(device_info, sign_key, sizeof(sign_key));
	memcpy(device_info + sizeof(sign_key), impl->device_id, id_len);
	memcpy(device_info + sizeof(sign_key) + id_len, impl->client_public_key,
			AP_ED25519_PUBLIC_KEY_LENGTH);

	if (ed25519_sign(signature, device_info, device_info_len, impl->client_private_key) < 0) {
		pw_log_error("pair-setup 3: ed25519_sign failed");
		return -1;
	}

	/* Inner sub-TLV: Identifier, PublicKey, Signature. */
	inner_tlv = tlv_new();
	inner_data_len = AP_REQUEST_BUFSIZE;
	inner_data = malloc(inner_data_len);
	if (inner_tlv == NULL || inner_data == NULL) {
		pw_log_error("pair-setup 3: out of memory");
		tlv_free(inner_tlv);
		free(inner_data);
		return -1;
	}
	tlv_add_value(inner_tlv, AP2_TLVType_Identifier, (const uint8_t *)impl->device_id, id_len);
	tlv_add_value(inner_tlv, AP2_TLVType_PublicKey, impl->client_public_key, AP_ED25519_PUBLIC_KEY_LENGTH);
	tlv_add_value(inner_tlv, AP2_TLVType_Signature, signature, sizeof(signature));

	ret = tlv_format(inner_tlv, inner_data, &inner_data_len);
	tlv_free(inner_tlv);
	if (ret < 0) {
		pw_log_error("pair-setup 3: inner tlv_format error");
		free(inner_data);
		return -1;
	}

	/* Encrypt the inner sub-TLV under the session key (nonce PS-Msg05). */
	memcpy(nonce + 4, "PS-Msg05", 8);
	encrypted_data_len = inner_data_len + sizeof(tag);
	encrypted_data = malloc(encrypted_data_len);
	if (encrypted_data == NULL) {
		pw_log_error("pair-setup 3: out of memory");
		free(inner_data);
		return -1;
	}
	ret = chacha20_poly1305_encrypt(encrypted_data, inner_data, inner_data_len,
			session_key, sizeof(session_key), NULL, 0, tag, sizeof(tag), nonce);
	free(inner_data);
	if (ret < 0) {
		pw_log_error("pair-setup 3: encryption failed");
		free(encrypted_data);
		return -1;
	}
	memcpy(encrypted_data + inner_data_len, tag, sizeof(tag));

	request = tlv_new();
	data = malloc(data_len);
	if (request == NULL || data == NULL) {
		pw_log_error("pair-setup 3: out of memory");
		tlv_free(request);
		free(data);
		free(encrypted_data);
		return -1;
	}
	tlv_add_value(request, AP2_TLVType_State, &state, sizeof(state));
	tlv_add_value(request, AP2_TLVType_EncryptedData, encrypted_data, encrypted_data_len);
	free(encrypted_data);

	ret = tlv_format(request, data, &data_len);
	tlv_free(request);
	if (ret < 0) {
		pw_log_error("pair-setup 3: tlv_format error");
		free(data);
		return -1;
	}

	pw_properties_set(impl->headers, "X-Apple-HKP", "3");
	ret = pw_rtsp_client_url_send(impl->rtsp, "/pair-setup", "POST", &impl->headers->dict,
			"application/octet-stream", data, data_len, rtsp_ap2_pair_setup3_reply, impl);
	free(data);
	return ret;
}

static int rtsp_do_ap2_pair_setup2(struct impl *impl)
{
	tlv_values_t *request;
	uint8_t *data;
	size_t data_len = AP_REQUEST_BUFSIZE;
	const char *auth_username = NULL;
	int ret;
	// TLV Pairing Message State
	uint8_t state = 0x03;

	data = malloc(data_len);
	request = tlv_new();
	if (data == NULL || request == NULL) {
		pw_log_error("pair-setup 2: out of memory");
		goto error;
	}

	// Calculate A
	srp_user_start_authentication(impl->srp_user, &auth_username, &impl->pkA, &impl->pkA_len);

	// Calculate M1 (client proof)
	srp_user_process_challenge(impl->srp_user, impl->salt, impl->salt_len, impl->pkB, impl->pkB_len, &impl->M1, &impl->M1_len);

	/* A degenerate server public key (B % N == 0) makes the SRP-6a safety
	 * check fail and leaves no client proof. Abort rather than sending an
	 * empty proof and continuing with an all-zero, attacker-known secret. */
	if (impl->M1 == NULL || impl->M1_len == 0) {
		pw_log_error("pair-setup 2: SRP produced no client proof; aborting");
		goto error;
	}

	tlv_add_value(request, AP2_TLVType_State, &state, sizeof(state));
	tlv_add_value(request, AP2_TLVType_PublicKey, impl->pkA, impl->pkA_len);
	tlv_add_value(request, AP2_TLVType_Proof, impl->M1, impl->M1_len);

	ret = tlv_format(request, data, &data_len);
	if (ret < 0) {
		pw_log_error("pair-setup 2: tlv_format returned an error");
		goto error;
	}

  	for (tlv_t *t=request->head; t; t=t->next) {
		pw_log_debug("pair-setup 2: sending TLV type %d (bytes: %zu)", t->type, t->size);
	}

	tlv_free(request);

   	ret = pw_rtsp_client_url_send(impl->rtsp, "/pair-setup", "POST", &impl->headers->dict,
				"application/octet-stream", data, data_len,
				rtsp_ap2_pair_setup2_reply, impl);
	free(data);
	return ret;

error:
	tlv_free(request);
	free(data);
	return -1;
}

static int rtsp_ap2_pair_setup1_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	uint32_t content_length;
	const char *len_str;
	tlv_values_t *response;
	tlv_t *pk;
	tlv_t *salt;

	pw_log_info("pair-setup (req 1) status: %d", status);

	if (status != 200) {
		pw_log_error("pair-setup (req 1): Server returned error %d", status);
		return -1;
	}

	if ((len_str = spa_dict_lookup(headers, "Content-Length")) == NULL) {
		pw_log_error("pair-setup (req 1) res: Missing Content-Length header");
		return -1;
	}
	
	if (!spa_atou32(len_str, &content_length, 0)) {
		pw_log_error("pair-setup (req 1) res: Could not read Content-Length header");
		return -1;
	}
	pw_log_debug("pair-setup (req 1) content len: %d", content_length);

	/* Bound the parse by the actually-buffered body: a peer-declared
	 * Content-Length larger than what we received would read past content->data. */
	response = tlv_message_process((const uint8_t *)content->data,
			SPA_MIN((size_t) content_length, content->size));
	if (!response) {
		pw_log_error("pair-setup (req 1) res: message process error");
		return -1;
	}

	pk = tlv_get_value(response, AP2_TLVType_PublicKey);
	if (!pk) {
		pw_log_error("pair-setup (req 1) res: missing public key");
		goto error;
	}
	if (pk->size > (long unsigned int) get_N_len(SRP_NG_3072)) {
		pw_log_error("pair-setup (req 1) res: invalid public key");
		goto error;
	}

	salt = tlv_get_value(response, AP2_TLVType_Salt);
	if (!salt) {
		pw_log_error("pair-setup (req 1) res: missing salt");
		goto error;
	}
	if (salt->size != 16) {
		pw_log_error("pair-setup (req 1) res: invalid salt");
		goto error;
	}

	impl->pkB_len = pk->size;
	impl->pkB = malloc(impl->pkB_len);
	impl->salt_len = salt->size;
	impl->salt = malloc(impl->salt_len);
	if (impl->pkB == NULL || impl->salt == NULL) {
		pw_log_error("pair-setup (req 1) res: out of memory");
		goto error;
	}
	memcpy(impl->pkB, pk->value, impl->pkB_len);
	memcpy(impl->salt, salt->value, impl->salt_len);

	tlv_free(response);

	return rtsp_do_ap2_pair_setup2(impl);

error:
	tlv_free(response);
	return -1;
}

/* The HomeKit controller pairing identifier must be identical at pair-setup
 * (when the accessory records our long-term public key under it) and at every
 * later pair-verify. Derive it deterministically from our long-term Ed25519
 * public key so it stays stable across module restarts without persisting a
 * separate value alongside the auth key. */
static void derive_device_id(struct impl *impl)
{
	snprintf(impl->device_id, sizeof(impl->device_id),
		"%02X:%02X:%02X:%02X:%02X:%02X",
		impl->client_public_key[0], impl->client_public_key[1],
		impl->client_public_key[2], impl->client_public_key[3],
		impl->client_public_key[4], impl->client_public_key[5]);
}

// Parse auth_key string from pair-setup: format is "<64 bytes client private key hex><32 bytes server public key hex>"
static int parse_auth_key(struct impl *impl, const char *auth_key)
{
	size_t len;

	if (!auth_key)
		return -1;

	len = strlen(auth_key);
	// Auth key can be just the private key (128 hex chars) or private key + server public key (192 hex chars)
	if (len == 2 * AP_ED25519_PRIVATE_KEY_LENGTH + 2 * AP_ED25519_PUBLIC_KEY_LENGTH) {
		hexread(impl->client_private_key, sizeof(impl->client_private_key), auth_key);
		hexread(impl->server_public_key, sizeof(impl->server_public_key),
			auth_key + 2 * AP_ED25519_PRIVATE_KEY_LENGTH);
		impl->have_server_public_key = true;
	} else if (len == 2 * AP_ED25519_PRIVATE_KEY_LENGTH) {
		hexread(impl->client_private_key, sizeof(impl->client_private_key), auth_key);
		memset(impl->server_public_key, 0, sizeof(impl->server_public_key));
		impl->have_server_public_key = false;
	} else {
		return -1;
	}

	// Extract public key from private key (Ed25519 private key contains the public key in the last 32 bytes)
	memcpy(impl->client_public_key, impl->client_private_key + AP_ED25519_PUBLIC_KEY_LENGTH,
	       AP_ED25519_PUBLIC_KEY_LENGTH);

	/* Restore the same controller pairing identifier the accessory recorded at
	 * pair-setup, so pair-verify presents a matching identity across restarts. */
	derive_device_id(impl);

	return 0;
}

static int rtsp_ap2_pair_verify2_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	tlv_values_t *response;
	uint32_t content_length;
	const char *str;

	pw_log_info("pair-verify (req 2) status: %d", status);

	if (status != 200) {
		pw_log_error("pair-verify (req 2): Server returned error %d", status);
		return -1;
	}

	if ((str = spa_dict_lookup(headers, "Content-Length")) == NULL) {
		pw_log_error("pair-verify (req 2) res: Missing Content-Length");
		return -1;
	}

	if (!spa_atou32(str, &content_length, 0)) {
		pw_log_error("pair-verify (req 2) res: Could not read Content-Length");
		return -1;
	}

	/* Bound the parse by the actually-buffered body: a peer-declared
	 * Content-Length larger than what we received would read past content->data. */
	response = tlv_message_process((const uint8_t *)content->data,
			SPA_MIN((size_t) content_length, content->size));
	if (!response) {
		pw_log_error("pair-verify (req 2) res: Received an error");
		return -1;
	}

	// The shared secret was already computed in pair-verify response 1.
	// Now derive and install the cipher context for encrypted communication.
	if (install_control_cipher(impl, AP_CONTROL_SALT, AP_CONTROL_ENC_INFO,
				AP_CONTROL_SALT, AP_CONTROL_DEC_INFO) < 0) {
		tlv_free(response);
		return -1;
	}

	pw_log_info("pair-verify completed, cipher context established");

	tlv_free(response);

	pw_properties_set(impl->headers, "X-Apple-HKP", NULL);

	return rtsp_do_setup(impl);
}

static int rtsp_do_ap2_pair_verify2(struct impl *impl)
{
	tlv_values_t *request;
	uint8_t *data;
	size_t data_len = AP_REQUEST_BUFSIZE;
	uint8_t state = 0x03;  // State M3
	uint8_t derived_key[32];
	uint8_t nonce[AP_NONCE_LENGTH] = { 0 };
	uint8_t tag[AP_AUTHTAG_LENGTH];
	uint8_t device_info[256];
	size_t device_info_len;
	uint8_t signature[AP_ED25519_SIGNATURE_LENGTH];
	uint8_t *encrypted_data = NULL;
	size_t encrypted_data_len;
	tlv_values_t *inner_tlv;
	uint8_t *inner_data;
	size_t inner_data_len;
	int ret;

	data = malloc(data_len);
	request = tlv_new();
	if (data == NULL || request == NULL) {
		pw_log_error("pair-verify 2: out of memory");
		goto error;
	}

	// Derive encryption key from shared secret
	// Apple devices use different salt/info strings
	const char *enc_salt = (impl->encryption == CRYPTO_PAIR_APPLE) ? 
				 AP_PAIR_VERIFY_APPLE_SALT : AP_PAIR_VERIFY_ENC_SALT;
	const char *enc_info = (impl->encryption == CRYPTO_PAIR_APPLE) ? 
				 AP_PAIR_VERIFY_APPLE_INFO : AP_PAIR_VERIFY_ENC_INFO;
	ret = hkdf_extract_expand(derived_key, sizeof(derived_key), 
					 impl->shared_secret, impl->shared_secret_len,
					 enc_salt, enc_info);
	if (ret < 0) {
		pw_log_error("pair-verify 2: hkdf error getting derived_key");
		goto error;
	}

	// Create device info: client_eph_public_key || device_id || server_eph_public_key
	device_info_len = sizeof(impl->client_eph_public_key) + strlen(impl->device_id) + sizeof(impl->server_eph_public_key);
	if (device_info_len > sizeof(device_info)) {
		pw_log_error("pair-verify 2: device_info buffer too small");
		goto error;
	}

	memcpy(device_info, impl->client_eph_public_key, sizeof(impl->client_eph_public_key));
	memcpy(device_info + sizeof(impl->client_eph_public_key), impl->device_id, strlen(impl->device_id));
	memcpy(device_info + sizeof(impl->client_eph_public_key) + strlen(impl->device_id), 
	       impl->server_eph_public_key, sizeof(impl->server_eph_public_key));

	// Sign device info
	ret = ed25519_sign(signature, device_info, device_info_len, impl->client_private_key);
	if (ret < 0) {
		pw_log_error("pair-verify 2: ed25519_sign failed");
		goto error;
	}

	// Create inner TLV with identifier and signature
	inner_tlv = tlv_new();
	inner_data_len = AP_REQUEST_BUFSIZE;
	inner_data = malloc(inner_data_len);
	if (inner_tlv == NULL || inner_data == NULL) {
		pw_log_error("pair-verify 2: out of memory");
		tlv_free(inner_tlv);
		free(inner_data);
		goto error;
	}
	tlv_add_value(inner_tlv, AP2_TLVType_Identifier, (const uint8_t *)impl->device_id, strlen(impl->device_id));
	tlv_add_value(inner_tlv, AP2_TLVType_Signature, signature, sizeof(signature));

	ret = tlv_format(inner_tlv, inner_data, &inner_data_len);
	tlv_free(inner_tlv);
	if (ret < 0) {
		pw_log_error("pair-verify 2: inner tlv_format returned an error");
		free(inner_data);
		goto error;
	}

	// Encrypt inner TLV
	memcpy(nonce + 4, "PV-Msg03", 8);

	encrypted_data_len = inner_data_len + sizeof(tag);
	encrypted_data = malloc(encrypted_data_len);
	if (encrypted_data == NULL) {
		pw_log_error("pair-verify 2: out of memory");
		free(inner_data);
		goto error;
	}

	ret = chacha20_poly1305_encrypt(encrypted_data, inner_data, inner_data_len,
					derived_key, sizeof(derived_key), NULL, 0, 
					tag, sizeof(tag), nonce);
	free(inner_data);
	if (ret < 0) {
		pw_log_error("pair-verify 2: chacha20 encryption failed");
		goto error;
	}

	memcpy(encrypted_data + inner_data_len, tag, sizeof(tag));

	// Build request TLV
	tlv_add_value(request, AP2_TLVType_State, &state, sizeof(state));
	tlv_add_value(request, AP2_TLVType_EncryptedData, encrypted_data, encrypted_data_len);

	ret = tlv_format(request, data, &data_len);
	if (ret < 0) {
		pw_log_error("pair-verify 2: tlv_format returned an error");
		goto error;
	}

	for (tlv_t *t = request->head; t; t = t->next) {
		pw_log_debug("pair-verify 2: sending TLV type %d (bytes: %zu)", t->type, t->size);
	}

	tlv_free(request);
	free(encrypted_data);

	pw_properties_set(impl->headers, "X-Apple-HKP", "3");

	ret = pw_rtsp_client_url_send(impl->rtsp, "/pair-verify", "POST", &impl->headers->dict,
				"application/octet-stream", data, data_len,
				rtsp_ap2_pair_verify2_reply, impl);
	free(data);
	return ret;

error:
	tlv_free(request);
	free(encrypted_data);
	free(data);
	return -1;
}

static int rtsp_ap2_pair_verify1_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	tlv_values_t *response;
	tlv_t *encrypted_data;
	tlv_t *public_key;
	uint32_t content_length;
	const char *str;
	uint8_t derived_key[32];
	uint8_t nonce[AP_NONCE_LENGTH] = { 0 };
	uint8_t tag[AP_AUTHTAG_LENGTH];
	size_t encrypted_len;
	uint8_t *decrypted_data = NULL;
	tlv_values_t *inner_response;
	tlv_t *device_id;
	tlv_t *signature;
	uint8_t device_info[256];
	size_t device_info_len;
	int ret;

	pw_log_info("pair-verify (req 1) status: %d", status);

	if (status != 200) {
		pw_log_error("pair-verify (req 1): Server returned error %d - auth key may be invalid", status);
		return -1;
	}

	if ((str = spa_dict_lookup(headers, "Content-Length")) == NULL) {
		pw_log_error("pair-verify (req 1) res: Missing Content-Length");
		return -1;
	}

	if (!spa_atou32(str, &content_length, 0)) {
		pw_log_error("pair-verify (req 1) res: Could not read Content-Length");
		return -1;
	}

	pw_log_debug("pair-verify (req 1) content len: %d", content_length);

	/* Bound the parse by the actually-buffered body: a peer-declared
	 * Content-Length larger than what we received would read past content->data. */
	response = tlv_message_process((const uint8_t *)content->data,
			SPA_MIN((size_t) content_length, content->size));
	if (!response) {
		pw_log_error("pair-verify (req 1) res: message process error");
		return -1;
	}

	// Get server's ephemeral public key
	public_key = tlv_get_value(response, AP2_TLVType_PublicKey);
	if (!public_key || public_key->size != AP_X25519_KEY_LENGTH) {
		pw_log_error("pair-verify (req 1) res: Missing or invalid public key");
		goto error;
	}

	memcpy(impl->server_eph_public_key, public_key->value, sizeof(impl->server_eph_public_key));

	// Compute shared secret
	ret = x25519_shared_secret(impl->shared_secret, impl->client_eph_private_key, impl->server_eph_public_key);
	if (ret < 0) {
		pw_log_error("pair-verify (req 1) res: x25519 shared secret derivation failed");
		goto error;
	}
	impl->shared_secret_len = AP_X25519_KEY_LENGTH;

	// Derive decryption key
	// Apple devices use different salt/info strings
	const char *enc_salt = (impl->encryption == CRYPTO_PAIR_APPLE) ? 
				 AP_PAIR_VERIFY_APPLE_SALT : AP_PAIR_VERIFY_ENC_SALT;
	const char *enc_info = (impl->encryption == CRYPTO_PAIR_APPLE) ? 
				 AP_PAIR_VERIFY_APPLE_INFO : AP_PAIR_VERIFY_ENC_INFO;
	ret = hkdf_extract_expand(derived_key, sizeof(derived_key),
					 impl->shared_secret, impl->shared_secret_len,
					 enc_salt, enc_info);
	if (ret < 0) {
		pw_log_error("pair-verify (req 1) res: hkdf error");
		goto error;
	}

	// Get encrypted data
	encrypted_data = tlv_get_value(response, AP2_TLVType_EncryptedData);
	if (!encrypted_data || encrypted_data->size < AP_AUTHTAG_LENGTH) {
		pw_log_error("pair-verify (req 1) res: Missing or invalid encrypted data");
		goto error;
	}

	encrypted_len = encrypted_data->size - AP_AUTHTAG_LENGTH;
	memcpy(tag, encrypted_data->value + encrypted_len, AP_AUTHTAG_LENGTH);
	memcpy(nonce + 4, "PV-Msg02", 8);

	decrypted_data = malloc(encrypted_len);
	if (decrypted_data == NULL) {
		pw_log_error("pair-verify (req 1) res: out of memory");
		goto error;
	}
	ret = chacha20_poly1305_decrypt(decrypted_data, encrypted_data->value, encrypted_len,
					derived_key, sizeof(derived_key), NULL, 0,
					tag, sizeof(tag), nonce);
	if (ret < 0) {
		pw_log_error("pair-verify (req 1) res: Decryption failed");
		goto error;
	}

	// Parse inner TLV
	inner_response = tlv_message_process(decrypted_data, encrypted_len);
	if (!inner_response) {
		pw_log_error("pair-verify (req 1) res: inner TLV parse error");
		goto error;
	}

	device_id = tlv_get_value(inner_response, AP2_TLVType_Identifier);
	signature = tlv_get_value(inner_response, AP2_TLVType_Signature);
	if (!device_id || !signature || signature->size != AP_ED25519_SIGNATURE_LENGTH) {
		pw_log_error("pair-verify (req 1) res: Missing identifier or signature");
		tlv_free(inner_response);
		goto error;
	}

	// Verify server signature (only if we have server's public key from pair-setup)
	if (impl->have_server_public_key) {
		// Create device info: server_eph_public_key || device_id || client_eph_public_key
		device_info_len = sizeof(impl->server_eph_public_key) + device_id->size + sizeof(impl->client_eph_public_key);
		if (device_info_len > sizeof(device_info)) {
			pw_log_error("pair-verify (req 1) res: device_info buffer too small");
			tlv_free(inner_response);
			goto error;
		}

		memcpy(device_info, impl->server_eph_public_key, sizeof(impl->server_eph_public_key));
		memcpy(device_info + sizeof(impl->server_eph_public_key), device_id->value, device_id->size);
		memcpy(device_info + sizeof(impl->server_eph_public_key) + device_id->size, 
		       impl->client_eph_public_key, sizeof(impl->client_eph_public_key));

		ret = ed25519_verify(signature->value, device_info, device_info_len, impl->server_public_key);
		if (ret < 0) {
			pw_log_error("pair-verify (req 1) res: Invalid server signature");
			tlv_free(inner_response);
			goto error;
		}
		pw_log_info("pair-verify (req 1): Server signature verified");
	} else {
		/* Should be unreachable: rtsp_do_ap2_pair_verify1 refuses to start
		 * without the accessory public key. Fail closed regardless. */
		pw_log_error("pair-verify (req 1): no server public key to verify signature; aborting");
		tlv_free(inner_response);
		goto error;
	}

	tlv_free(inner_response);
	free(decrypted_data);
	tlv_free(response);

	return rtsp_do_ap2_pair_verify2(impl);

error:
	free(decrypted_data);
	tlv_free(response);
	return -1;
}

static int rtsp_do_ap2_pair_verify1(struct impl *impl)
{
	tlv_values_t *request;
	uint8_t *data;
	size_t data_len = AP_REQUEST_BUFSIZE;
	uint8_t state = 0x01;  // State M1
	int ret;

	// Parse the stored auth key
	ret = parse_auth_key(impl, impl->auth_key);
	if (ret < 0) {
		pw_log_error("pair-verify 1: Invalid auth key format");
		return -1;
	}

	/* Fail closed: without the accessory's long-term public key we cannot
	 * verify its signature in M2, which would leave the X25519 exchange
	 * unauthenticated and open to an active MITM. Refuse to send our
	 * ephemeral key with a key-less (private-key-only) auth blob. */
	if (!impl->have_server_public_key) {
		pw_log_error("pair-verify 1: stored auth key has no accessory public key; "
				"cannot authenticate the device. Re-pair to obtain a full key.");
		return -1;
	}

	// Generate ephemeral X25519 key pair
	ret = x25519_keypair(impl->client_eph_public_key, impl->client_eph_private_key);
	if (ret < 0) {
		pw_log_error("pair-verify 1: x25519 keygen failed");
		return -1;
	}

	data = malloc(data_len);
	request = tlv_new();
	if (data == NULL || request == NULL) {
		pw_log_error("pair-verify 1: out of memory");
		goto error;
	}

	tlv_add_value(request, AP2_TLVType_State, &state, sizeof(state));
	tlv_add_value(request, AP2_TLVType_PublicKey, impl->client_eph_public_key, sizeof(impl->client_eph_public_key));

	ret = tlv_format(request, data, &data_len);
	if (ret < 0) {
		pw_log_error("pair-verify 1: tlv_format returned an error");
		goto error;
	}

	for (tlv_t *t = request->head; t; t = t->next) {
		pw_log_debug("pair-verify 1: sending TLV type %d value (%zu bytes)", t->type, t->size);
	}

	tlv_free(request);

	pw_properties_set(impl->headers, "X-Apple-HKP", "3");

	ret = pw_rtsp_client_url_send(impl->rtsp, "/pair-verify", "POST", &impl->headers->dict,
				"application/octet-stream", data, data_len,
				rtsp_ap2_pair_verify1_reply, impl);
	free(data);
	return ret;

error:
	tlv_free(request);
	free(data);
	return -1;
}

static int rtsp_do_ap2_pair_setup1(struct impl *impl)
{
	tlv_values_t *request;
	uint8_t *data;
	size_t data_len;
	// TLV Pairing Method: Pair Setup
	uint8_t method = 0x00;
	// TLV Pairing Flags: Transient
	uint8_t flags = 0x10;
	// TLV Pairing Message State
	uint8_t state = 0x01;
	/* Transient pairing uses the fixed PIN 3939 and ephemeral keys. Full
	 * (HomeKit/Apple) pairing uses the PIN shown on the Apple device and
	 * produces a persistent authorization key. */
	const char *pin = impl->pairing_pin ? impl->pairing_pin : "3939";
	int ret;

	data_len = AP_REQUEST_BUFSIZE;
	data = malloc(data_len);
	request = tlv_new();
	if (data == NULL || request == NULL) {
		pw_log_error("pair-setup 1: out of memory");
		goto error;
	}

  	impl->srp_user = srp_user_new(SRP_SHA512, SRP_NG_3072, AP_SRP_USER_NAME, (unsigned char *)pin, strlen(pin), 0, 0);
  	if (!impl->srp_user) {
		pw_log_error("pair-setup 1: SRP user creation failed");
		goto error;
	}

   	tlv_add_value(request, AP2_TLVType_State, &state, sizeof(state));
  	tlv_add_value(request, AP2_TLVType_Method, &method, sizeof(method));
	if (!impl->pairing_pin)
		tlv_add_value(request, AP2_TLVType_Flags, &flags, sizeof(flags));

  	ret = tlv_format(request, data, &data_len);
  	if (ret < 0) {
		pw_log_error("pair-setup 1: tlv_format returned an error");
		goto error;
	}

  	for (tlv_t *t=request->head; t; t=t->next) {
		pw_log_debug("pair-setup 1: sending TLV type %d value (%zu bytes)", t->type, t->size);
	}

  	tlv_free(request);

	pw_properties_set(impl->headers, "X-Apple-HKP", impl->pairing_pin ? "3" : "4");

	ret = pw_rtsp_client_url_send(impl->rtsp, "/pair-setup", "POST", &impl->headers->dict,
				"application/octet-stream", (unsigned char *)data, data_len,
				rtsp_ap2_pair_setup1_reply, impl);
	free(data);
	return ret;
error:
	tlv_free(request);
	free(data);
	return -1;
}

/* On-screen PIN pairing for receivers that display their code on their own
 * screen (LG webOS TVs and similar) rather than expecting a fixed/known PIN.
 * POST /pair-pin-start makes the code appear; pair-setup then runs with the code
 * the user reads off the screen. The code is bound to this RTSP connection and
 * lives ~30s, so it must be supplied and pair-setup M1 sent on the same
 * connection with no GET /info keepalive in between (that resets the receiver's
 * pairing context). A daemon cannot prompt interactively, so the code is fed in
 * two ways: the idiomatic one is the "default" metadata "raop.pairing-pin" key
 * (set by a UI, a session manager, or `pw-metadata <id> raop.pairing-pin 1234`);
 * as a fallback it is polled from the raop.pairing-pin-file. */
#define AP_PIN_WAIT_TICKS 28

/* A PIN arrived (from metadata or the file): stop waiting and resume pair-setup
 * on this same connection with the supplied code. */
static void ap2_pairing_pin_received(struct impl *impl, const char *pin, const char *src)
{
	if (!impl->pin_waiting)
		return;
	impl->pin_waiting = false;
	pw_timer_queue_cancel(&impl->pin_timer);
	pw_log_info("Received on-screen PIN via %s; continuing pair-setup", src);
	free(impl->pairing_pin);
	impl->pairing_pin = strdup(pin);
	if (impl->pairing_pin == NULL || rtsp_do_ap2_pair_setup1(impl) < 0)
		pw_impl_module_schedule_destroy(impl->module);
}

static void rtsp_ap2_pin_poll(void *data)
{
	struct impl *impl = data;
	char buf[64];
	FILE *f;

	if (!impl->pin_waiting)
		return;

	f = impl->pairing_pin_file ? fopen(impl->pairing_pin_file, "re") : NULL;
	if (f != NULL) {
		char *pin = NULL;
		if (fgets(buf, sizeof(buf), f) != NULL) {
			buf[strcspn(buf, "\r\n")] = '\0';
			if (buf[0] != '\0')
				pin = buf;
		}
		fclose(f);
		if (pin != NULL) {
			ap2_pairing_pin_received(impl, pin, impl->pairing_pin_file);
			return;
		}
	}

	if (--impl->pin_wait_ticks <= 0) {
		impl->pin_waiting = false;
		pw_log_error("Timed out waiting for the on-screen PIN (set the \"raop.pairing-pin\" "
				"metadata key or write it to '%s')",
				impl->pairing_pin_file ? impl->pairing_pin_file : "(unset)");
		pw_impl_module_schedule_destroy(impl->module);
		return;
	}

	pw_timer_queue_add(impl->timer_queue, &impl->pin_timer, NULL,
			SPA_NSEC_PER_SEC, rtsp_ap2_pin_poll, impl);
}

static int metadata_property(void *data, uint32_t subject, const char *key,
		const char *type, const char *value)
{
	struct impl *impl = data;

	if (impl->pin_waiting && value != NULL && spa_streq(key, "raop.pairing-pin"))
		ap2_pairing_pin_received(impl, value, "metadata");
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct impl *impl = data;

	if (impl->metadata != NULL || !spa_streq(type, PW_TYPE_INTERFACE_Metadata))
		return;
	if (props == NULL || !spa_streq(spa_dict_lookup(props, "metadata.name"), "default"))
		return;

	impl->metadata = pw_registry_bind(impl->registry, id, type, PW_VERSION_METADATA, 0);
	if (impl->metadata != NULL) {
		pw_metadata_add_listener(impl->metadata, &impl->metadata_listener,
				&metadata_events, impl);
		impl->metadata_id = id;
	}
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;

	if (impl->metadata != NULL && id == impl->metadata_id) {
		spa_hook_remove(&impl->metadata_listener);
		pw_proxy_destroy((struct pw_proxy *)impl->metadata);
		impl->metadata = NULL;
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static int rtsp_ap2_pair_pin_start_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;

	pw_log_info("pair-pin-start status: %d", status);
	if (status != 200) {
		pw_log_error("pair-pin-start failed with status %d", status);
		return -EIO;
	}

	pw_log_info("A pairing code should now be shown on the receiver's screen. "
			"Supply it within %d seconds via the \"raop.pairing-pin\" metadata key "
			"(e.g. `pw-metadata 0 raop.pairing-pin <code>`) or by writing it to '%s'.",
			AP_PIN_WAIT_TICKS,
			impl->pairing_pin_file ? impl->pairing_pin_file : "(raop.pairing-pin-file unset)");

	impl->pin_waiting = true;
	impl->pin_wait_ticks = AP_PIN_WAIT_TICKS;
	pw_timer_queue_add(impl->timer_queue, &impl->pin_timer, NULL,
			SPA_NSEC_PER_SEC, rtsp_ap2_pin_poll, impl);
	return 0;
}

static int rtsp_do_ap2_pair_pin_start(struct impl *impl)
{
	/* Watch the "default" metadata for the PIN the receiver is about to show;
	 * this is what lets a UI or `pw-metadata` feed the code at runtime. */
	if (impl->registry == NULL) {
		impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
		if (impl->registry != NULL)
			pw_registry_add_listener(impl->registry, &impl->registry_listener,
					&registry_events, impl);
	}
	/* Persistent (HomeKit) pairing selector, as sent before pair-setup. */
	pw_properties_set(impl->headers, "X-Apple-HKP", "3");
	return pw_rtsp_client_url_send(impl->rtsp, "/pair-pin-start", "POST",
			&impl->headers->dict, NULL, NULL, 0,
			rtsp_ap2_pair_pin_start_reply, impl);
}

static int rtsp_raop_options_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res = 0;

	pw_log_info("options status: %d", status);

	switch (status) {
	case 401:
		res = rtsp_do_raop_auth(impl, headers);
		break;
	case 200:
		if (impl->encryption == CRYPTO_AUTH_SETUP)
			res = rtsp_do_raop_auth_setup(impl);
		else
			res = rtsp_do_raop_announce(impl);
		break;
	default:
		pw_impl_module_schedule_destroy(impl->module);
		return 0;
	}
	return res;
}

static void rtsp_do_raop_options(void *data)
{
	struct impl *impl = data;
	uint8_t rac[16];
	char sac[16*4];

	if (pw_getrandom(rac, sizeof(rac), 0) < 0) {
		pw_log_error("error generating random data: %m");
		return;
	}

	pw_base64_encode(rac, sizeof(rac), sac, '\0');
	pw_properties_set(impl->headers, "Apple-Challenge", sac);

	pw_rtsp_client_send(impl->rtsp, "OPTIONS", &impl->headers->dict,
		NULL, NULL, rtsp_raop_options_reply, impl);

	return;
}

// Status flags from device info (from openairplay spec)
#define AIRPLAY_FLAG_PIN_REQUIRED                   (1 << 3)
#define AIRPLAY_FLAG_PASSWORD_REQUIRED              (1 << 7)
#define AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED      (1 << 9)
#define AIRPLAY_FLAG_SETUP_HK_ACCESS_CTRL           (1 << 10)

static int rtsp_get_info_reply(void *data, int status, const struct spa_dict *headers, const struct pw_array *content)
{
	struct impl *impl = data;
	int res;

	pw_log_info("info status: %d", status);
	plist_t info;
	res = wplist_from_bin(&info, content);
	if (res < 0)
		return res; 

	plist_t features_plist = plist_dict_get_item(info, "features");
	plist_t status_flags_plist = plist_dict_get_item(info, "statusFlags");
	if (!features_plist || !status_flags_plist) {
		pw_log_error("GET /info: missing features or statusFlags");
		plist_free(info);
		return -1;
	}

	/* plist_get_uint_val leaves the value untouched for a non-integer node,
	 * so start from 0 rather than reading uninitialised memory. */
	uint64_t features = 0, status_flags = 0;
	plist_get_uint_val(features_plist, &features);
	plist_get_uint_val(status_flags_plist, &status_flags);
	pw_log_info("features value: 0x%" PRIx64, features);
	pw_log_info("statusFlags value: 0x%" PRIx64 " (PIN:%d, Password:%d, OneTimePairing:%d)",
		    status_flags,
		    (int)!!(status_flags & AIRPLAY_FLAG_PIN_REQUIRED),
		    (int)!!(status_flags & AIRPLAY_FLAG_PASSWORD_REQUIRED),
		    (int)!!(status_flags & AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED));

	plist_free(info);

	/* Initial pairing: when a PIN is configured, run the full pair-setup to
	 * mint and store a persistent authorization key (HomeKit/Apple devices),
	 * regardless of the selected pair mode. */
	if (impl->pairing_pin) {
		if (spa_streq(impl->pairing_pin, "WAIT")) {
			pw_log_info("Starting AirPlay pairing: requesting the on-screen PIN via /pair-pin-start");
			return rtsp_do_ap2_pair_pin_start(impl);
		}
		pw_log_info("Starting AirPlay pairing with configured PIN");
		return rtsp_do_ap2_pair_setup1(impl);
	}

	// Determine the appropriate pairing mode based on configuration and device
	// status. Propagate a failed kickoff so dispatch_handler surfaces it as a
	// terminal error and the sink is torn down rather than left wedged.
	switch (impl->encryption) {
	case CRYPTO_PAIR_HOMEKIT:
		// HomeKit pairing for 3rd party devices with persistent keys
		if (!impl->auth_key) {
			pw_log_error("pair_homekit requested but no auth_key provided");
			return -1;
		}
		pw_log_info("Using pair-verify with HomeKit parameters (3rd party device)");
		return rtsp_do_ap2_pair_verify1(impl);
	case CRYPTO_PAIR_APPLE:
		// Apple pairing for Apple devices (Apple TV, HomePod)
		if (!impl->auth_key) {
			pw_log_error("pair_apple requested but no auth_key provided");
			return -1;
		}
		pw_log_info("Using pair-verify with Apple parameters (Apple device)");
		return rtsp_do_ap2_pair_verify1(impl);
	case CRYPTO_PAIR_TRANSIENT:
		// Transient pairing for 3rd party devices (no persistent keys)
		if (status_flags & AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED) {
			pw_log_warn("Device requires persistent pairing but transient was requested");
			if (impl->auth_key) {
				pw_log_info("Falling back to pair-verify with stored auth key");
				impl->encryption = CRYPTO_PAIR_HOMEKIT;
				return rtsp_do_ap2_pair_verify1(impl);
			}
			pw_log_error("Device requires pairing but no auth_key provided. Please pair first.");
			return -1;
		}
		if (status_flags & AIRPLAY_FLAG_PIN_REQUIRED) {
			pw_log_error("Device requires PIN authentication - transient pairing not supported");
			return -1;
		}
		pw_log_info("Using transient pairing (3rd party device)");
		return rtsp_do_ap2_pair_setup1(impl);
	default:
		rtsp_do_raop_options(impl);
		return 0;
	}
}

static void rtsp_connected(void *data)
{
	struct impl *impl = data;
	uint32_t sci[2];

	pw_log_info("connected");

	impl->connected = true;

	if (pw_getrandom(sci, sizeof(sci), 0) < 0) {
		pw_log_error("error generating random data: %m");
		return;
	}

	if (IS_AP2_ENCRYPTION(impl->encryption)) {
		pw_properties_setf(impl->headers, "DACP-ID",
			"%08X%08X", sci[0], sci[1]);
		pw_properties_set(impl->headers, "User-Agent", AP_USER_AGENT);

		/* AirPlay 2: GET /info first to get device capabilities, then pair-setup or pair-verify */
		pw_rtsp_client_url_send(impl->rtsp, "/info", "GET", &impl->headers->dict,
			NULL, NULL, 0,
			rtsp_get_info_reply, impl);
	} else {
		/* Legacy RAOP: Send OPTIONS with Apple-Challenge */
		pw_properties_setf(impl->headers, "Client-Instance",
			"%08X%08X", sci[0], sci[1]);
		pw_properties_setf(impl->headers, "DACP-ID",
			"%08X%08X", sci[0], sci[1]);
		pw_properties_set(impl->headers, "User-Agent", DEFAULT_USER_NAME "/" PACKAGE_VERSION);
		rtsp_do_raop_options(impl);
	}

	return;
}

/* Tear the module down at most once. Fatal connection/handshake errors can
 * arrive via several paths (socket error, reply-handler failure, disconnect);
 * without a guard they would each schedule a separate destroy. */
static void schedule_destroy_once(struct impl *impl)
{
	if (impl->destroying)
		return;
	impl->destroying = true;
	pw_impl_module_schedule_destroy(impl->module);
}

static void connection_cleanup(struct impl *impl)
{
	impl->ready = false;
	impl->auth_setup_auth_retry = false;
	/* Stop the encode/send path: without this, stream_send_packet keeps
	 * encrypting and writing to the now-closed server_fd after a drop. */
	impl->recording = false;

	/* Per-session SRP pair-setup state is re-created on the next attempt;
	 * release it here so a reconnect (or repeated pair-setup) does not leak. */
	if (impl->srp_user != NULL) {
		srp_user_free(impl->srp_user);
		impl->srp_user = NULL;
	}
	free(impl->pkB);
	impl->pkB = NULL;
	if (impl->salt != NULL) {
		explicit_bzero(impl->salt, impl->salt_len);
		free(impl->salt);
		impl->salt = NULL;
	}
	if (impl->M2 != NULL) {
		explicit_bzero(impl->M2, impl->M2_len);
		free(impl->M2);
		impl->M2 = NULL;
	}

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
	pw_timer_queue_cancel(&impl->feedback_timer);
	pw_timer_queue_cancel(&impl->handshake_timer);
	/* Also stop the AP2 PIN poll: a disconnect mid-pairing would otherwise
	 * leave it firing rtsp_ap2_pin_poll() against a torn-down connection. */
	pw_timer_queue_cancel(&impl->pin_timer);

	rtsp_clear_auth(impl);
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
	struct impl *impl = data;
	pw_log_error("error %d (%s)", res, spa_strerror(res));
	/* A socket-level error or a failed reply handler is terminal: tear the
	 * sink down so it does not linger connected-but-silent. For auto-created
	 * sinks module-raop-discover will respawn a fresh one while the device is
	 * still advertised. */
	schedule_destroy_once(impl);
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

static void stream_report_error(void *data, const char *error)
{
	struct impl *impl = data;
	if (error) {
		pw_log_error("stream error: %s", error);
		pw_impl_module_schedule_destroy(impl->module);
	}
}

/* Bound the whole connect + handshake window. AirPlay 2 adds ~10 sequential
 * round-trips; a device that accepts the TCP connection and then stalls (or
 * under-delivers a Content-Length body) would otherwise wedge the sink forever.
 * Cancelled once RECORD succeeds and in connection_cleanup. */
#define AP_HANDSHAKE_TIMEOUT_SEC 15

static void rtsp_handshake_timeout(void *data)
{
	struct impl *impl = data;
	pw_log_error("timed out waiting for the device to complete the RTSP handshake");
	schedule_destroy_once(impl);
}

static void arm_handshake_timeout(struct impl *impl)
{
	pw_timer_queue_cancel(&impl->handshake_timer);
	pw_timer_queue_add(impl->timer_queue, &impl->handshake_timer, NULL,
			AP_HANDSHAKE_TIMEOUT_SEC * SPA_NSEC_PER_SEC,
			rtsp_handshake_timeout, impl);
}

static int rtsp_do_connect(struct impl *impl)
{
	const char *hostname, *port;
	uint32_t session_id;
	int res;

	if (impl->connected) {
		if (!impl->ready) {
			/* Re-establish the session on the existing connection. AirPlay 2
			 * restarts from GET /info (pairing is still valid); legacy RAOP
			 * re-announces. */
			arm_handshake_timeout(impl);
			if (IS_AP2_ENCRYPTION(impl->encryption))
				return pw_rtsp_client_url_send(impl->rtsp, "/info", "GET",
						&impl->headers->dict, NULL, NULL, 0,
						rtsp_get_info_reply, impl);
			return rtsp_do_raop_announce(impl);
		}
		return 0;
	}

	arm_handshake_timeout(impl);

	hostname = pw_properties_get(impl->props, "raop.ip");
	port = pw_properties_get(impl->props, "raop.port");
	if (hostname == NULL || port == NULL)
		return -EINVAL;

	if ((res = pw_getrandom(&session_id, sizeof(session_id), 0)) < 0)
		return res;

	spa_scnprintf(impl->session_id, sizeof(impl->session_id), "%u", session_id);

	uint32_t port_num;
	if (!spa_atou32(port, &port_num, 10) || port_num == 0 || port_num > 65535)
		return -EINVAL;

	return pw_rtsp_client_connect(impl->rtsp, hostname, (uint16_t)port_num, impl->session_id);
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

	if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
		return;

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
		{
			bool mute = false;
			if (spa_pod_get_bool(&prop->value, &mute) == 0) {
				if (impl->mute != mute) {
					impl->mute = mute;
					rtsp_send_volume(impl);
				}
                        }
			uint32_t flags = prop->flags & ~SPA_POD_PROP_FLAG_HARDWARE;
			spa_pod_builder_prop(&b, SPA_PROP_softMute, 0);
			spa_pod_builder_bool(&b, impl->disable_volume && mute);
			spa_pod_builder_prop(&b, prop->key, flags);
			spa_pod_builder_raw_padded(&b, &prop->value, prop->value.size);
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t i, n_vols;
			float vols[MAX_CHANNELS], volume;
			float soft_vols[MAX_CHANNELS];

			if ((n_vols = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vols, SPA_N_ELEMENTS(vols))) > 0) {
				volume = 0.0f;
				for (i = 0; i < n_vols; i++)
				{
					volume += vols[i];
					soft_vols[i] = impl->disable_volume ? vols[i] : 1.0f;
				}
				volume /= n_vols;
				volume = SPA_CLAMPF(cbrtf(volume) * 30 - 30, VOLUME_MIN, VOLUME_MAX);
				impl->volume = volume;
				impl->volume_valid = true;

				rtsp_send_volume(impl);
				spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
				spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
						n_vols, soft_vols);
			}
			uint32_t flags = prop->flags & ~SPA_POD_PROP_FLAG_HARDWARE;
			spa_pod_builder_prop(&b, prop->key, flags);
			spa_pod_builder_raw_padded(&b, &prop->value, prop->value.size);
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
	.report_error = stream_report_error,
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
	if (impl->metadata) {
		spa_hook_remove(&impl->metadata_listener);
		impl->metadata = NULL;
	}
	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		impl->registry = NULL;
	}
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	/* Cancel any pending timers first: their pw_timer structs are embedded in
	 * impl, so letting one fire after the free would be a use-after-free. */
	pw_timer_queue_cancel(&impl->feedback_timer);
	pw_timer_queue_cancel(&impl->handshake_timer);
	pw_timer_queue_cancel(&impl->pin_timer);

	/* Metadata/registry proxies are children of impl->core, so drop them
	 * before disconnecting it. */
	if (impl->metadata) {
		spa_hook_remove(&impl->metadata_listener);
		pw_proxy_destroy((struct pw_proxy *)impl->metadata);
		impl->metadata = NULL;
	}
	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy *)impl->registry);
		impl->registry = NULL;
	}

	if (impl->stream)
		rtp_stream_destroy(impl->stream);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->rtsp)
		pw_rtsp_client_destroy(impl->rtsp);

	/* The control cipher is owned here; the RTSP client only borrowed it.
	 * It holds the live ChaCha20-Poly1305 control-channel keys: wipe first. */
	if (impl->cipher_ctx) {
		explicit_bzero(impl->cipher_ctx, sizeof(*impl->cipher_ctx));
		free(impl->cipher_ctx);
	}

	if (impl->ctx)
		EVP_CIPHER_CTX_free(impl->ctx);

	if (impl->srp_user)
		srp_user_free(impl->srp_user);

	free(impl->pkB);
	if (impl->M2 != NULL) {
		explicit_bzero(impl->M2, impl->M2_len);
		free(impl->M2);
	}
	if (impl->salt != NULL) {
		explicit_bzero(impl->salt, impl->salt_len);
		free(impl->salt);
	}

	pw_properties_free(impl->headers);
	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);
	if (impl->password) {
		explicit_bzero(impl->password, strlen(impl->password));
		free(impl->password);
	}
	if (impl->auth_key) {
		/* The reusable long-term Ed25519 controller credential. */
		explicit_bzero(impl->auth_key, strlen(impl->auth_key));
		free(impl->auth_key);
	}
	if (impl->pairing_pin) {
		explicit_bzero(impl->pairing_pin, strlen(impl->pairing_pin));
		free(impl->pairing_pin);
	}
	free(impl->pairing_pin_file);
	/* Wipe the long-term and session secrets still living in impl. */
	explicit_bzero(impl->shared_secret, sizeof(impl->shared_secret));
	explicit_bzero(impl->private_key, sizeof(impl->private_key));
	explicit_bzero(impl->client_private_key, sizeof(impl->client_private_key));
	explicit_bzero(impl->client_eph_private_key, sizeof(impl->client_eph_private_key));
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

	impl->srp_user = NULL;

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
	impl->timer_queue = pw_context_get_timer_queue(context);

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
	else if (spa_streq(str, "fp_sap25"))
		impl->encryption = CRYPTO_FP_SAP25;
	else if (spa_streq(str, "pair_setup") || spa_streq(str, "pair_transient"))
		impl->encryption = CRYPTO_PAIR_TRANSIENT;
	else if (spa_streq(str, "pair_homekit"))
		impl->encryption = CRYPTO_PAIR_HOMEKIT;
	else if (spa_streq(str, "pair_apple"))
		impl->encryption = CRYPTO_PAIR_APPLE;
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

	impl->disable_volume = pw_properties_get_bool(props, "raop.disable-volume", false);

	str = pw_properties_get(props, "raop.auth_key");
	impl->auth_key = str ? strdup(str) : NULL;

	/* If no explicit key was given but raop.auth-key-file exists and is
	 * readable, load the key that a previous pair-setup wrote there. This lets
	 * a once-paired device keep working across restarts without hand-copying
	 * the key into stream.rules. */
	if (impl->auth_key == NULL &&
	    (str = pw_properties_get(props, "raop.auth-key-file")) != NULL) {
		FILE *f = fopen(str, "re");
		if (f != NULL) {
			char keybuf[2 * (AP_ED25519_PRIVATE_KEY_LENGTH + AP_ED25519_PUBLIC_KEY_LENGTH) + 2] = { 0 };
			if (fgets(keybuf, sizeof(keybuf), f) != NULL) {
				keybuf[strcspn(keybuf, "\r\n")] = '\0';
				if (keybuf[0] != '\0') {
					impl->auth_key = strdup(keybuf);
					pw_log_info("Loaded authorization key from '%s'", str);
				}
			}
			explicit_bzero(keybuf, sizeof(keybuf));
			fclose(f);
		}
	}

	str = pw_properties_get(props, "raop.pairing-pin");
	impl->pairing_pin = str ? strdup(str) : NULL;

	str = pw_properties_get(props, "raop.pairing-pin-file");
	impl->pairing_pin_file = str ? strdup(str) : NULL;
	/* "WAIT" (on-screen code) needs a file to read the code from; default it. */
	if (impl->pairing_pin != NULL && spa_streq(impl->pairing_pin, "WAIT") &&
	    impl->pairing_pin_file == NULL)
		impl->pairing_pin_file = strdup("/tmp/pipewire-raop-pin");

	/* Fallback controller pairing identifier. For persistent (HomeKit/Apple)
	 * pairing this is replaced by derive_device_id() once the long-term key is
	 * minted (pair-setup) or restored (parse_auth_key), so it stays stable
	 * across restarts; transient pairing does not use an Identifier. */
	{
		uint8_t mac_bytes[6];
		if (pw_getrandom(mac_bytes, sizeof(mac_bytes), 0) >= 0) {
			snprintf(impl->device_id, sizeof(impl->device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
				mac_bytes[0], mac_bytes[1], mac_bytes[2],
				mac_bytes[3], mac_bytes[4], mac_bytes[5]);
		}
	}

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

	if (pw_properties_get(props, PW_KEY_AUDIO_FORMAT) == NULL)
		pw_properties_setf(props, PW_KEY_AUDIO_FORMAT, "%s", RAOP_FORMAT);

	impl->stride = RAOP_STRIDE;

	if ((str = pw_properties_get(props, PW_KEY_AUDIO_RATE)) == NULL)
		pw_properties_setf(props, PW_KEY_AUDIO_RATE, "%u", impl->rate);
	else if (!spa_atou32(str, &impl->rate, 10))
		impl->rate = RAOP_RATE;

	impl->latency = msec_to_samples(impl, RAOP_LATENCY_MS);
	if ((str = pw_properties_get(props, "raop.latency.ms")) == NULL)
		str = SPA_STRINGIFY(DEFAULT_LATENCY_MS);
	uint32_t latency_ms;
	if (spa_atou32(str, &latency_ms, 10))
		impl->latency = SPA_MAX(impl->latency, msec_to_samples(impl, latency_ms));

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
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
				impl->psamples, impl->rate);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(props, PW_KEY_NODE_NETWORK, "true");
	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	if (pw_properties_get(props, "net.mtu") == NULL)
		pw_properties_set(props, "net.mtu", "1448");
	if (pw_properties_get(props, "rtp.sender-ts-offset") == NULL)
		pw_properties_setf(props, "rtp.sender-ts-offset", "%d", 0);
	if (pw_properties_get(props, "sess.ts-direct") == NULL)
		pw_properties_setf(props, "sess.ts-direct", "%d", 0);
	if (pw_properties_get(props, "sess.media") == NULL)
		pw_properties_set(props, "sess.media", "raop");
	if (pw_properties_get(props, "sess.latency.msec") == NULL)
		pw_properties_setf(props, "sess.latency.msec", "%d", impl->latency * 1000 / impl->rate);

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(impl->stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_LAYOUT);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_DEVICE_ICON_NAME);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_NETWORK);
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

	impl->mtu = SPA_MIN(pw_properties_get_uint32(impl->props, "net.mtu", 1448), 9000u);
	impl->sync_period = impl->rate * impl->stride / impl->mtu;
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
