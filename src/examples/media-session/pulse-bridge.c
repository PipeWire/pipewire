/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#if HAVE_PWD_H
#include <pwd.h>
#endif

#include <spa/utils/result.h>
#include <spa/debug/dict.h>
#include <spa/debug/mem.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/pod.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include "pipewire/pipewire.h"

#include "media-session.h"

#define NAME		"pulse-bridge"
#define SESSION_KEY	"pulse-bridge"

#define FLAG_SHMDATA			0x80000000LU
#define FLAG_SHMDATA_MEMFD_BLOCK	0x20000000LU
#define FLAG_SHMRELEASE			0x40000000LU
#define FLAG_SHMREVOKE			0xC0000000LU
#define FLAG_SHMMASK			0xFF000000LU
#define FLAG_SEEKMASK			0x000000FFLU
#define FLAG_SHMWRITABLE		0x00800000LU

#define FRAME_SIZE_MAX_ALLOW (1024*1024*16)

#define PROTOCOL_FLAG_MASK	0xffff0000u
#define PROTOCOL_VERSION_MASK	0x0000ffffu
#define PROTOCOL_VERSION	34

#define NATIVE_COOKIE_LENGTH 256
#define MAX_TAG_SIZE (64*1024)

#define MIN_BUFFERS     8u
#define MAX_BUFFERS     64u

enum error_code {
	ERR_OK = 0,			/**< No error */
	ERR_ACCESS,			/**< Access failure */
	ERR_COMMAND,			/**< Unknown command */
	ERR_INVALID,			/**< Invalid argument */
	ERR_EXIST,			/**< Entity exists */
	ERR_NOENTITY,			/**< No such entity */
	ERR_CONNECTIONREFUSED,		/**< Connection refused */
	ERR_PROTOCOL,			/**< Protocol error */
	ERR_TIMEOUT,			/**< Timeout */
	ERR_AUTHKEY,			/**< No authentication key */
	ERR_INTERNAL,			/**< Internal error */
	ERR_CONNECTIONTERMINATED,	/**< Connection terminated */
	ERR_KILLED,			/**< Entity killed */
	ERR_INVALIDSERVER,		/**< Invalid server */
	ERR_MODINITFAILED,		/**< Module initialization failed */
	ERR_BADSTATE,			/**< Bad state */
	ERR_NODATA,			/**< No data */
	ERR_VERSION,			/**< Incompatible protocol version */
	ERR_TOOLARGE,			/**< Data too large */
	ERR_NOTSUPPORTED,		/**< Operation not supported \since 0.9.5 */
	ERR_UNKNOWN,			/**< The error code was unknown to the client */
	ERR_NOEXTENSION,		/**< Extension does not exist. \since 0.9.12 */
	ERR_OBSOLETE,			/**< Obsolete functionality. \since 0.9.15 */
	ERR_NOTIMPLEMENTED,		/**< Missing implementation. \since 0.9.15 */
	ERR_FORKED,			/**< The caller forked without calling execve() and tried to reuse the context. \since 0.9.15 */
	ERR_IO,				/**< An IO error happened. \since 0.9.16 */
	ERR_BUSY,			/**< Device or resource busy. \since 0.9.17 */
	ERR_MAX				/**< Not really an error but the first invalid error code */
};

struct impl;

struct descriptor {
	uint32_t length;
	uint32_t channel;
	uint32_t offset_hi;
	uint32_t offset_lo;
	uint32_t flags;
};

enum {
	TAG_INVALID = 0,
	TAG_STRING = 't',
	TAG_STRING_NULL = 'N',
	TAG_U32 = 'L',
	TAG_U8 = 'B',
	TAG_U64 = 'R',
	TAG_S64 = 'r',
	TAG_SAMPLE_SPEC = 'a',
	TAG_ARBITRARY = 'x',
	TAG_BOOLEAN_TRUE = '1',
	TAG_BOOLEAN_FALSE = '0',
	TAG_BOOLEAN = TAG_BOOLEAN_TRUE,
	TAG_TIMEVAL = 'T',
	TAG_USEC = 'U'  /* 64bit unsigned */,
	TAG_CHANNEL_MAP = 'm',
	TAG_CVOLUME = 'v',
	TAG_PROPLIST = 'P',
	TAG_VOLUME = 'V',
	TAG_FORMAT_INFO = 'f',
};

struct message {
	struct spa_list link;
	uint32_t channel;
	uint32_t allocated;
	uint32_t length;
	uint32_t offset;
	uint8_t *data;
};

struct client {
	struct spa_list link;
	struct impl *impl;

        struct spa_source *source;

	uint32_t version;

	struct pw_properties *props;

	struct pw_core *core;

	uint32_t in_index;
	uint32_t out_index;
	struct descriptor desc;
	struct message *message;

	struct pw_map streams;
	struct spa_list free_messages;
	struct spa_list out_messages;
};

enum sample_format {
	SAMPLE_U8,
	SAMPLE_ALAW,
	SAMPLE_ULAW,
	SAMPLE_S16LE,
	SAMPLE_S16BE,
	SAMPLE_FLOAT32LE,
	SAMPLE_FLOAT32BE,
	SAMPLE_S32LE,
	SAMPLE_S32BE,
	SAMPLE_S24LE,
	SAMPLE_S24BE,
	SAMPLE_S24_32LE,
	SAMPLE_S24_32BE,
	SAMPLE_MAX,
	SAMPLE_INVALID = -1
};

struct format {
	uint32_t format;
	const char *name;
	uint32_t size;
};

static const struct format audio_formats[] = {
	[SAMPLE_U8] = { SPA_AUDIO_FORMAT_U8, "u8", 1 },
	[SAMPLE_ALAW] = { SPA_AUDIO_FORMAT_UNKNOWN, "alaw", 1 },
	[SAMPLE_ULAW] = { SPA_AUDIO_FORMAT_UNKNOWN, "ulaw", 1 },
	[SAMPLE_S16LE] = { SPA_AUDIO_FORMAT_S16_LE, "s16le", 2 },
	[SAMPLE_S16BE] = { SPA_AUDIO_FORMAT_S16_BE, "s16be", 2 },
	[SAMPLE_FLOAT32LE] = { SPA_AUDIO_FORMAT_F32_LE, "f32le", 4 },
	[SAMPLE_FLOAT32BE] = { SPA_AUDIO_FORMAT_F32_BE, "f32be", 5 },
	[SAMPLE_S32LE] = { SPA_AUDIO_FORMAT_S32_LE, "s32le", 4 },
	[SAMPLE_S32BE] = { SPA_AUDIO_FORMAT_S32_BE, "s32be", 4 },
	[SAMPLE_S24LE] = { SPA_AUDIO_FORMAT_S24_LE, "s24le", 3 },
	[SAMPLE_S24BE] = { SPA_AUDIO_FORMAT_S24_BE, "s24be", 3 },
	[SAMPLE_S24_32LE] = { SPA_AUDIO_FORMAT_S24_32_LE, "s24_32le", 4 },
	[SAMPLE_S24_32BE] = { SPA_AUDIO_FORMAT_S24_32_BE, "s24_32be", 4 },
};

static inline uint32_t format_pa2id(enum sample_format format)
{
	if (format < 0 || (size_t)format >= SPA_N_ELEMENTS(audio_formats))
		return SPA_AUDIO_FORMAT_UNKNOWN;
	return audio_formats[format].format;
}

static inline enum sample_format format_id2pa(uint32_t id)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_formats); i++) {
		if (id == audio_formats[i].format)
			return i;
	}
	return SAMPLE_INVALID;
}


struct sample_spec {
	enum sample_format format;
	uint32_t rate;
	uint8_t channels;
};

static inline uint32_t sample_spec_frame_size(const struct sample_spec *ss)
{
	if (ss->format < 0 || (size_t)ss->format >= SPA_N_ELEMENTS(audio_formats))
		return SPA_AUDIO_FORMAT_UNKNOWN;
	return audio_formats[ss->format].size * ss->channels;
}

#define CHANNELS_MAX	64

struct channel_map {
	uint8_t channels;
	uint32_t map[CHANNELS_MAX];
};

struct cvolume {
	uint8_t channels;
	float values[CHANNELS_MAX];
};

enum encoding {
	ENCODING_ANY,
	ENCODING_PCM,
	ENCODING_AC3_IEC61937,
	ENCODING_EAC3_IEC61937,
	ENCODING_MPEG_IEC61937,
	ENCODING_DTS_IEC61937,
	ENCODING_MPEG2_AAC_IEC61937,
	ENCODING_TRUEHD_IEC61937,
	ENCODING_DTSHD_IEC61937,
	ENCODING_MAX,
	NCODING_INVALID = -1,
};

struct format_info {
	enum encoding encoding;
	struct pw_properties *props;
};

struct device {
	uint32_t index;
	char *name;
	enum pw_direction direction;
	struct pw_properties *props;
	struct sample_spec ss;
	struct cvolume volume;
	struct channel_map map;
	bool muted;
};

struct buffer_attr {
	uint32_t maxlength;
	uint32_t tlength;
	uint32_t prebuf;
	uint32_t minreq;
	uint32_t fragsize;
};

struct stream {
	uint32_t create_tag;
	uint32_t channel;	/* index in map */

	struct impl *impl;
	struct client *client;
	enum pw_direction direction;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_list messages;
	int64_t read_index;
	int64_t write_index;
	uint64_t underrun_for;
	uint64_t playing_for;
	uint64_t ticks_base;
	struct timeval timestamp;
	int64_t delay;

	struct sample_spec ss;
	struct channel_map map;
	struct buffer_attr attr;
	uint32_t frame_size;

	struct cvolume volume;
	bool muted;

	uint32_t drain_tag;
	unsigned int corked:1;
	unsigned int volume_set:1;
	unsigned int muted_set:1;
	unsigned int adjust_latency:1;
	unsigned int have_time:1;
};

struct impl {
	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_loop *loop;
	struct pw_context *context;
        struct spa_source *source;

	struct spa_list clients;

	struct device default_sink;
	struct device default_source;
};


enum {
	/* Generic commands */
	COMMAND_ERROR,
	COMMAND_TIMEOUT, /* pseudo command */
	COMMAND_REPLY,

	/* CLIENT->SERVER */
	COMMAND_CREATE_PLAYBACK_STREAM,        /* Payload changed in v9, v12 (0.9.0, 0.9.8) */
	COMMAND_DELETE_PLAYBACK_STREAM,
	COMMAND_CREATE_RECORD_STREAM,          /* Payload changed in v9, v12 (0.9.0, 0.9.8) */
	COMMAND_DELETE_RECORD_STREAM,
	COMMAND_EXIT,
	COMMAND_AUTH,
	COMMAND_SET_CLIENT_NAME,
	COMMAND_LOOKUP_SINK,
	COMMAND_LOOKUP_SOURCE,
	COMMAND_DRAIN_PLAYBACK_STREAM,
	COMMAND_STAT,
	COMMAND_GET_PLAYBACK_LATENCY,
	COMMAND_CREATE_UPLOAD_STREAM,
	COMMAND_DELETE_UPLOAD_STREAM,
	COMMAND_FINISH_UPLOAD_STREAM,
	COMMAND_PLAY_SAMPLE,
	COMMAND_REMOVE_SAMPLE,

	COMMAND_GET_SERVER_INFO,
	COMMAND_GET_SINK_INFO,
	COMMAND_GET_SINK_INFO_LIST,
	COMMAND_GET_SOURCE_INFO,
	COMMAND_GET_SOURCE_INFO_LIST,
	COMMAND_GET_MODULE_INFO,
	COMMAND_GET_MODULE_INFO_LIST,
	COMMAND_GET_CLIENT_INFO,
	COMMAND_GET_CLIENT_INFO_LIST,
	COMMAND_GET_SINK_INPUT_INFO,          /* Payload changed in v11 (0.9.7) */
	COMMAND_GET_SINK_INPUT_INFO_LIST,     /* Payload changed in v11 (0.9.7) */
	COMMAND_GET_SOURCE_OUTPUT_INFO,
	COMMAND_GET_SOURCE_OUTPUT_INFO_LIST,
	COMMAND_GET_SAMPLE_INFO,
	COMMAND_GET_SAMPLE_INFO_LIST,
	COMMAND_SUBSCRIBE,

	COMMAND_SET_SINK_VOLUME,
	COMMAND_SET_SINK_INPUT_VOLUME,
	COMMAND_SET_SOURCE_VOLUME,

	COMMAND_SET_SINK_MUTE,
	COMMAND_SET_SOURCE_MUTE,

	COMMAND_CORK_PLAYBACK_STREAM,
	COMMAND_FLUSH_PLAYBACK_STREAM,
	COMMAND_TRIGGER_PLAYBACK_STREAM,

	COMMAND_SET_DEFAULT_SINK,
	COMMAND_SET_DEFAULT_SOURCE,

	COMMAND_SET_PLAYBACK_STREAM_NAME,
	COMMAND_SET_RECORD_STREAM_NAME,

	COMMAND_KILL_CLIENT,
	COMMAND_KILL_SINK_INPUT,
	COMMAND_KILL_SOURCE_OUTPUT,

	COMMAND_LOAD_MODULE,
	COMMAND_UNLOAD_MODULE,

	/* Obsolete */
	COMMAND_ADD_AUTOLOAD___OBSOLETE,
	COMMAND_REMOVE_AUTOLOAD___OBSOLETE,
	COMMAND_GET_AUTOLOAD_INFO___OBSOLETE,
	COMMAND_GET_AUTOLOAD_INFO_LIST___OBSOLETE,

	COMMAND_GET_RECORD_LATENCY,
	COMMAND_CORK_RECORD_STREAM,
	COMMAND_FLUSH_RECORD_STREAM,
	COMMAND_PREBUF_PLAYBACK_STREAM,

	/* SERVER->CLIENT */
	COMMAND_REQUEST,
	COMMAND_OVERFLOW,
	COMMAND_UNDERFLOW,
	COMMAND_PLAYBACK_STREAM_KILLED,
	COMMAND_RECORD_STREAM_KILLED,
	COMMAND_SUBSCRIBE_EVENT,

	/* A few more client->server commands */

	/* Supported since protocol v10 (0.9.5) */
	COMMAND_MOVE_SINK_INPUT,
	COMMAND_MOVE_SOURCE_OUTPUT,

	/* Supported since protocol v11 (0.9.7) */
	COMMAND_SET_SINK_INPUT_MUTE,

	COMMAND_SUSPEND_SINK,
	COMMAND_SUSPEND_SOURCE,

	/* Supported since protocol v12 (0.9.8) */
	COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR,
	COMMAND_SET_RECORD_STREAM_BUFFER_ATTR,

	COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE,
	COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE,

	/* SERVER->CLIENT */
	COMMAND_PLAYBACK_STREAM_SUSPENDED,
	COMMAND_RECORD_STREAM_SUSPENDED,
	COMMAND_PLAYBACK_STREAM_MOVED,
	COMMAND_RECORD_STREAM_MOVED,

	/* Supported since protocol v13 (0.9.11) */
	COMMAND_UPDATE_RECORD_STREAM_PROPLIST,
	COMMAND_UPDATE_PLAYBACK_STREAM_PROPLIST,
	COMMAND_UPDATE_CLIENT_PROPLIST,
	COMMAND_REMOVE_RECORD_STREAM_PROPLIST,
	COMMAND_REMOVE_PLAYBACK_STREAM_PROPLIST,
	COMMAND_REMOVE_CLIENT_PROPLIST,

	/* SERVER->CLIENT */
	COMMAND_STARTED,

	/* Supported since protocol v14 (0.9.12) */
	COMMAND_EXTENSION,
	/* Supported since protocol v15 (0.9.15) */
	COMMAND_GET_CARD_INFO,
	COMMAND_GET_CARD_INFO_LIST,
	COMMAND_SET_CARD_PROFILE,

	COMMAND_CLIENT_EVENT,
	COMMAND_PLAYBACK_STREAM_EVENT,
	COMMAND_RECORD_STREAM_EVENT,

	/* SERVER->CLIENT */
	COMMAND_PLAYBACK_BUFFER_ATTR_CHANGED,
	COMMAND_RECORD_BUFFER_ATTR_CHANGED,

	/* Supported since protocol v16 (0.9.16) */
	COMMAND_SET_SINK_PORT,
	COMMAND_SET_SOURCE_PORT,

	/* Supported since protocol v22 (1.0) */
	COMMAND_SET_SOURCE_OUTPUT_VOLUME,
	COMMAND_SET_SOURCE_OUTPUT_MUTE,

	/* Supported since protocol v27 (3.0) */
	COMMAND_SET_PORT_LATENCY_OFFSET,

	/* Supported since protocol v30 (6.0) */
	/* BOTH DIRECTIONS */
	COMMAND_ENABLE_SRBCHANNEL,
	COMMAND_DISABLE_SRBCHANNEL,

	/* Supported since protocol v31 (9.0)
	 * BOTH DIRECTIONS */
	COMMAND_REGISTER_MEMFD_SHMID,

	COMMAND_MAX
};
struct command {
	const char *name;
	int (*run) (struct client *client, uint32_t command, uint32_t tag, struct message *msg);
};
static const struct command commands[COMMAND_MAX];

static int message_get(struct message *m, ...);

static int read_u8(struct message *m, uint8_t *val)
{
	if (m->offset + 1 > m->length)
		return -ENOSPC;
	*val = m->data[m->offset];
	m->offset++;
	return 0;
}

static int read_u32(struct message *m, uint32_t *val)
{
	if (m->offset + 4 > m->length)
		return -ENOSPC;
	memcpy(val, &m->data[m->offset], 4);
	*val = ntohl(*val);
	m->offset += 4;
	return 0;
}
static int read_u64(struct message *m, uint64_t *val)
{
	uint32_t tmp;
	int res;
	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	*val = ((uint64_t)tmp) << 32;
	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	*val |= tmp;
	return 0;
}

static int read_sample_spec(struct message *m, struct sample_spec *ss)
{
	int res;
	uint8_t tmp;
	if ((res = read_u8(m, &tmp)) < 0)
		return res;
	ss->format = tmp;
	if ((res = read_u8(m, &ss->channels)) < 0)
		return res;
	return read_u32(m, &ss->rate);
}

static int read_props(struct message *m, struct pw_properties *props)
{
	int res;

	while (true) {
		char *key;
		void *data;
		uint32_t length;

		if ((res = message_get(m,
				TAG_STRING, &key,
				TAG_INVALID)) < 0)
			return res;

		if (key == NULL)
			break;

		if ((res = message_get(m,
				TAG_U32, &length,
				TAG_INVALID)) < 0)
			return res;
		if (length > MAX_TAG_SIZE)
			return -EINVAL;

		if ((res = message_get(m,
				TAG_ARBITRARY, &data, length,
				TAG_INVALID)) < 0)
			return res;

		pw_log_debug("%s %s", key, (char*)data);
		pw_properties_set(props, key, data);
	}
	return 0;
}

static int read_arbitrary(struct message *m, const void **val, size_t length)
{
	uint32_t len;
	int res;
	if ((res = read_u32(m, &len)) < 0)
		return res;
	if (len != length)
		return -EINVAL;
	if (m->offset + length > m->length)
		return -ENOSPC;
	*val = m->data + m->offset;
	m->offset += length;
	return 0;
}

static int read_string(struct message *m, char **str)
{
	uint32_t n, maxlen = m->length - m->offset;
	n = strnlen(m->data + m->offset, maxlen);
	if (n == maxlen)
		return -EINVAL;
	*str = m->data + m->offset;
	m->offset += n + 1;
	return 0;
}

static int read_timeval(struct message *m, struct timeval *tv)
{
	int res;
	uint32_t tmp;

	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	tv->tv_sec = tmp;
	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	tv->tv_usec = tmp;
	return 0;
}

static int read_channel_map(struct message *m, struct channel_map *map)
{
	int res;
	uint8_t i, tmp;

	if ((res = read_u8(m, &map->channels)) < 0)
		return res;
	if (map->channels > CHANNELS_MAX)
		return -EINVAL;
	for (i = 0; i < map->channels; i ++) {
		if ((res = read_u8(m, &tmp)) < 0)
			return res;
		map->map[i] = tmp;
	}
	return 0;
}
static int read_volume(struct message *m, float *vol)
{
	int res;
	uint32_t v;
	if ((res = read_u32(m, &v)) < 0)
		return res;
	*vol = ((float)v) / 0x10000U;
	return 0;
}

static int read_cvolume(struct message *m, struct cvolume *vol)
{
	int res;
	uint8_t i;

	if ((res = read_u8(m, &vol->channels)) < 0)
		return res;
	if (vol->channels > CHANNELS_MAX)
		return -EINVAL;
	for (i = 0; i < vol->channels; i ++) {
		if ((res = read_volume(m, &vol->values[i])) < 0)
			return res;
	}
	return 0;
}

static int read_format_info(struct message *m, struct format_info *info)
{
	int res;
	uint8_t tag, encoding;

	if ((res = read_u8(m, &tag)) < 0)
		return res;
	if (tag != TAG_U8)
		return -EPROTO;
	if ((res = read_u8(m, &encoding)) < 0)
		return res;
	info->encoding = encoding;

	if ((res = read_u8(m, &tag)) < 0)
		return res;
	if (tag != TAG_PROPLIST)
		return -EPROTO;

	info->props = pw_properties_new(NULL, NULL);
	if (info->props == NULL)
		return -errno;
	return read_props(m, info->props);
}

static int message_get(struct message *m, ...)
{
	va_list va;
	int res;

	va_start(va, m);

	while (true) {
		int tag = va_arg(va, int);
		uint8_t dtag;
		if (tag == TAG_INVALID)
			break;

		if ((res = read_u8(m, &dtag)) < 0)
			return res;

		switch (dtag) {
		case TAG_STRING:
			if (tag != TAG_STRING)
				return -EINVAL;
			if ((res = read_string(m, va_arg(va, char**))) < 0)
				return res;
			break;
		case TAG_STRING_NULL:
			if (tag != TAG_STRING)
				return -EINVAL;
			*va_arg(va, char**) = NULL;
			break;
		case TAG_U8:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u8(m, va_arg(va, uint8_t*))) < 0)
				return res;
			break;
		case TAG_U32:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u32(m, va_arg(va, uint32_t*))) < 0)
				return res;
			break;
		case TAG_S64:
		case TAG_U64:
		case TAG_USEC:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u64(m, va_arg(va, uint64_t*))) < 0)
				return res;
			break;
		case TAG_SAMPLE_SPEC:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_sample_spec(m, va_arg(va, struct sample_spec*))) < 0)
				return res;
			break;
		case TAG_ARBITRARY:
		{
			const void **val = va_arg(va, const void**);
			size_t len = va_arg(va, size_t);
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_arbitrary(m, val, len)) < 0)
				return res;
			break;
		}
		case TAG_BOOLEAN_TRUE:
			if (tag != TAG_BOOLEAN)
				return -EINVAL;
			*va_arg(va, bool*) = true;
			break;
		case TAG_BOOLEAN_FALSE:
			if (tag != TAG_BOOLEAN)
				return -EINVAL;
			*va_arg(va, bool*) = false;
			break;
		case TAG_TIMEVAL:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_timeval(m, va_arg(va, struct timeval*))) < 0)
				return res;
			break;
		case TAG_CHANNEL_MAP:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_channel_map(m, va_arg(va, struct channel_map*))) < 0)
				return res;
			break;
		case TAG_CVOLUME:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_cvolume(m, va_arg(va, struct cvolume*))) < 0)
				return res;
			break;
		case TAG_PROPLIST:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_props(m, va_arg(va, struct pw_properties*))) < 0)
				return res;
			break;
		case TAG_VOLUME:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_volume(m, va_arg(va, float*))) < 0)
				return res;
			break;
		case TAG_FORMAT_INFO:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_format_info(m, va_arg(va, struct format_info*))) < 0)
				return res;
			break;
		}
	}
	va_end(va);

	return 0;
}

static void write_8(struct message *m, uint8_t val)
{
	if (m->length < m->allocated)
		m->data[m->length] = val;
	m->length++;
}

static void write_32(struct message *m, uint32_t val)
{
	val = htonl(val);
	if (m->length + 4 <= m->allocated)
		memcpy(m->data + m->length, &val, 4);
	m->length += 4;
}

static void write_string(struct message *m, const char *s)
{
	write_8(m, s ? TAG_STRING : TAG_STRING_NULL);
	if (s != NULL) {
		int len = strlen(s) + 1;
		if (m->length + len <= m->allocated)
			strcpy(&m->data[m->length], s);
		m->length += len;
	}
}
static void write_u8(struct message *m, uint8_t val)
{
	write_8(m, TAG_U8);
	write_8(m, val);
}

static void write_u32(struct message *m, uint32_t val)
{
	write_8(m, TAG_U32);
	write_32(m, val);
}

static void write_64(struct message *m, uint8_t tag, uint64_t val)
{
	write_8(m, tag);
	write_32(m, val >> 32);
	write_32(m, val);
}

static void write_sample_spec(struct message *m, struct sample_spec *ss)
{
	write_8(m, TAG_SAMPLE_SPEC);
	write_8(m, ss->format);
	write_8(m, ss->channels);
	write_32(m, ss->rate);
}

static void write_arbitrary(struct message *m, const void *p, size_t length)
{
	write_8(m, TAG_ARBITRARY);
	write_32(m, length);
	if (length > 0 && m->length + length <= m->allocated)
		memcpy(m->data + m->length, p, length);
	m->length += length;
}

static void write_boolean(struct message *m, bool val)
{
	write_8(m, val ? TAG_BOOLEAN_TRUE : TAG_BOOLEAN_FALSE);
}

static void write_timeval(struct message *m, struct timeval *tv)
{
	write_8(m, TAG_TIMEVAL);
	write_32(m, tv->tv_sec);
	write_32(m, tv->tv_usec);
}

static void write_channel_map(struct message *m, struct channel_map *map)
{
	uint8_t i;
	write_8(m, TAG_CHANNEL_MAP);
	write_8(m, map->channels);
	for (i = 0; i < map->channels; i ++)
		write_8(m, map->map[i]);
}

static void write_volume(struct message *m, float vol)
{
	write_8(m, TAG_VOLUME);
	write_32(m, vol * 0x10000U);
}

static void write_cvolume(struct message *m, struct cvolume *cvol)
{
	uint8_t i;
	write_8(m, TAG_CVOLUME);
	write_8(m, cvol->channels);
	for (i = 0; i < cvol->channels; i ++)
		write_32(m, cvol->values[i] * 0x10000U);
}

static void write_props(struct message *m, struct pw_properties *props)
{
	const struct spa_dict_item *it;
	write_8(m, TAG_PROPLIST);
	if (props != NULL) {
		spa_dict_for_each(it, &props->dict) {
			int l = strlen(it->value);
			write_string(m, it->key);
			write_u32(m, l+1);
			write_arbitrary(m, it->value, l+1);
		}
	}
	write_string(m, NULL);
}

static void write_format_info(struct message *m, struct format_info *info)
{
	write_8(m, TAG_FORMAT_INFO);
	write_u8(m, (uint8_t) info->encoding);
	write_props(m, info->props);
}

static int message_put(struct message *m, ...)
{
	va_list va;

	va_start(va, m);

	while (true) {
		int tag = va_arg(va, int);
		if (tag == TAG_INVALID)
			break;

		switch (tag) {
		case TAG_STRING:
			write_string(m, va_arg(va, const char *));
			break;
		case TAG_U8:
			write_u8(m, (uint8_t)va_arg(va, int));
			break;
		case TAG_U32:
			write_u32(m, (uint32_t)va_arg(va, uint32_t));
			break;
		case TAG_S64:
		case TAG_U64:
		case TAG_USEC:
			write_64(m, tag, va_arg(va, uint64_t));
			break;
		case TAG_SAMPLE_SPEC:
			write_sample_spec(m, va_arg(va, struct sample_spec*));
			break;
		case TAG_ARBITRARY:
		{
			const void *p = va_arg(va, const void*);
			size_t length = va_arg(va, size_t);
			write_arbitrary(m, p, length);
			break;
		}
		case TAG_BOOLEAN:
			write_boolean(m, va_arg(va, int));
			break;
		case TAG_TIMEVAL:
			write_timeval(m, va_arg(va, struct timeval*));
			break;
		case TAG_CHANNEL_MAP:
			write_channel_map(m, va_arg(va, struct channel_map*));
			break;
		case TAG_CVOLUME:
			write_cvolume(m, va_arg(va, struct cvolume*));
			break;
		case TAG_PROPLIST:
			write_props(m, va_arg(va, struct pw_properties*));
			break;
		case TAG_VOLUME:
			write_volume(m, va_arg(va, double));
			break;
		case TAG_FORMAT_INFO:
			write_format_info(m, va_arg(va, struct format_info*));
			break;
		}
	}
	va_end(va);

	return 0;
}

static void message_free(struct client *client, struct message *msg, bool destroy)
{
	spa_list_remove(&msg->link);
	if (destroy)
		free(msg);
	else
		spa_list_append(&client->free_messages, &msg->link);
}

static struct message *message_alloc(struct client *client, uint32_t size, uint32_t channel)
{
	struct message *msg = NULL;

	if (!spa_list_is_empty(&client->free_messages)) {
		msg = spa_list_first(&client->free_messages, struct message, link);
		spa_list_remove(&msg->link);
	}
	if (msg == NULL || msg->allocated < size) {
		uint32_t alloc = SPA_ROUND_UP_N(SPA_MAX(size, 4096u), 4096u);
		msg = realloc(msg, sizeof(struct message) + alloc);
		msg->allocated = alloc;
		msg->data = SPA_MEMBER(msg, sizeof(struct message), void);
	}
	msg->channel = channel;
	msg->offset = 0;
	msg->length = size;
	return msg;
}

static int flush_messages(struct client *client)
{
	int res;

	while (true) {
		struct message *m;
		struct descriptor desc;
		void *data;
		size_t size;

		if (spa_list_is_empty(&client->out_messages))
			return 0;
		m = spa_list_first(&client->out_messages, struct message, link);

		if (client->out_index < sizeof(desc)) {
			desc.length = htonl(m->length);
			desc.channel = htonl(m->channel);
			desc.offset_hi = 0;
			desc.offset_lo = 0;
			desc.flags = 0;

			data = SPA_MEMBER(&desc, client->out_index, void);
			size = sizeof(desc) - client->out_index;
		} else if (client->out_index < m->length + sizeof(desc)) {
			uint32_t idx = client->out_index - sizeof(desc);
			data = m->data + idx;
			size = m->length - idx;
		} else {
			spa_list_remove(&m->link);
			spa_list_append(&client->free_messages, &m->link);
			client->out_index = 0;
			continue;
		}

		while (true) {
			res = send(client->source->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (res < 0) {
				pw_log_info("send channel:%d %zu, res %d: %m", m->channel, size, res);
				if (errno == EINTR)
					continue;
				else
					return -errno;
			}
			client->out_index += res;
			break;
		}
	}
	return 0;
}

static int send_message(struct client *client, struct message *m)
{
	struct impl *impl = client->impl;
	int res;

	m->offset = 0;
	spa_list_append(&client->out_messages, &m->link);
	res = flush_messages(client);
	if (res == -EAGAIN) {
		int mask = client->source->mask;
		SPA_FLAG_SET(mask, SPA_IO_OUT);
		pw_loop_update_io(impl->loop, client->source, mask);
		res = 0;
	}
	return res;
}

static struct message *reply_new(struct client *client, uint32_t tag)
{
	struct message *reply;
	reply = message_alloc(client, 0, -1);
	pw_log_debug(NAME" %p: REPLY tag:%u", client, tag);
	message_put(reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);
	return reply;
}

static int reply_simple_ack(struct client *client, uint32_t tag)
{
	struct message *reply = reply_new(client, tag);
	return send_message(client, reply);
}

static int reply_error(struct client *client, uint32_t tag, uint32_t error)
{
	struct message *reply;

	pw_log_debug(NAME" %p: ERROR tag:%u error:%u", client, tag, error);

	reply = message_alloc(client, 0, -1);
	message_put(reply,
		TAG_U32, COMMAND_ERROR,
		TAG_U32, tag,
		TAG_U32, error,
		TAG_INVALID);
	return send_message(client, reply);
}

static int do_command_auth(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t version;
	const void *cookie;
	int res;

	if ((res = message_get(m,
			TAG_U32, &version,
			TAG_ARBITRARY, &cookie, NATIVE_COOKIE_LENGTH,
			TAG_INVALID)) < 0) {
		return res;
	}
	if (version < 8)
		return -EPROTO;

	if ((version & PROTOCOL_VERSION_MASK) >= 13)
		version &= PROTOCOL_VERSION_MASK;

	client->version = version;

	pw_log_info(NAME" %p: AUTH version:%d", impl, version);

	reply = reply_new(client, tag);
	message_put(reply,
			TAG_U32, PROTOCOL_VERSION,
			TAG_INVALID);

	return send_message(client, reply);
}

static int do_set_client_name(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	const char *name = NULL;
	int res, changed = 0;

	if (client->version < 13) {
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			return res;
		if (name)
			changed += pw_properties_set(client->props, "application.name", name);
	} else {
		if ((res = message_get(m,
				TAG_PROPLIST, client->props,
				TAG_INVALID)) < 0)
			return res;
		changed++;
	}
	if (changed)
		pw_core_update_properties(client->core, &client->props->dict);

	pw_log_info(NAME" %p: SET_CLIENT_NAME %s", impl,
			pw_properties_get(client->props, "application.name"));

	reply = reply_new(client, tag);

	if (client->version >= 13) {
		message_put(reply,
			TAG_U32, 0,	/* client index */
			TAG_INVALID);
	}
	return send_message(client, reply);
}

static int do_subscribe(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t mask;
	int res;

	if ((res = message_get(m,
			TAG_U32, &mask,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: SUBSCRIBE mask:%08x", impl, mask);

	reply = reply_new(client, tag);

	return send_message(client, reply);
}

static void stream_flush(struct stream *stream)
{
	struct message *msg;
	spa_list_consume(msg, &stream->messages, link)
		message_free(stream->client, msg, false);
	stream->write_index = stream->read_index = 0;
	stream->playing_for = 0;
	stream->underrun_for = 0;
	stream->have_time = false;
}

static void stream_free(struct stream *stream)
{
	struct client *client = stream->client;
	pw_map_remove(&client->streams, stream->channel);
	stream_flush(stream);
	if (stream->stream) {
		spa_hook_remove(&stream->stream_listener);
		pw_stream_destroy(stream->stream);
	}
	free(stream);
}

static inline uint32_t queued_size(const struct stream *s, uint64_t elapsed)
{
	uint64_t queued;
	queued = s->write_index - SPA_MIN(s->read_index, s->write_index);
	queued -= SPA_MIN(queued, elapsed);
	return queued;
}

static inline uint32_t target_queue(const struct stream *s)
{
	return s->attr.tlength;
}

static inline uint32_t wanted_size(const struct stream *s, uint32_t queued, uint32_t target)
{
	return target - SPA_MIN(queued, target);
}

static inline uint32_t required_size(const struct stream *s)
{
	return s->attr.minreq;
}

static inline uint32_t writable_size(const struct stream *s, uint64_t elapsed)
{
	uint32_t queued, target, wanted, required;

	queued = queued_size(s, elapsed);
	target = target_queue(s);
	wanted = wanted_size(s, queued, target);
	required = required_size(s);

	pw_log_trace("stream %p, queued:%u target:%u wanted:%u required:%u",
			s, queued, target, wanted, required);

	if (s->adjust_latency)
		if (queued >= wanted)
			wanted = 0;
	if (wanted < required)
		wanted = 0;

	return wanted;
}

static int send_command_request(struct stream *stream)
{
	struct client *client = stream->client;
	struct message *msg;
	uint32_t size;

	size = writable_size(stream, 0);
	if (size == 0)
		return 0;

	pw_log_trace(NAME" %p: REQUEST channel:%d %u", stream, stream->channel, size);

	msg = message_alloc(client, 0, -1);
	message_put(msg,
		TAG_U32, COMMAND_REQUEST,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_U32, size,
		TAG_INVALID);

	return send_message(client, msg);
}

static int reply_create_playback_stream(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t size;

	size = writable_size(stream, 0);

	reply = reply_new(client, stream->create_tag);
	message_put(reply,
		TAG_U32, stream->channel,	/* stream index/channel */
		TAG_U32, stream->channel,	/* sink_input/stream index */
		TAG_U32, size,			/* missing/requested bytes */
		TAG_INVALID);

	if (client->version >= 9) {
		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_INVALID);
	}
	if (client->version >= 12) {
		message_put(reply,
			TAG_SAMPLE_SPEC, &stream->ss,
			TAG_CHANNEL_MAP, &stream->map,
			TAG_U32, impl->default_sink.index,		/* sink index */
			TAG_STRING, impl->default_sink.name,		/* sink name */
			TAG_BOOLEAN, false,			/* sink suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		message_put(reply,
			TAG_USEC, 0ULL,			/* sink configured latency */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(reply,
			TAG_FORMAT_INFO, &info,		/* sink_input format */
			TAG_INVALID);
	}

	stream->create_tag = SPA_ID_INVALID;

	return send_message(client, reply);
}

static int reply_create_record_stream(struct stream *stream)
{
	struct client *client = stream->client;
	struct impl *impl = client->impl;
	struct message *reply;

	reply = reply_new(client, stream->create_tag);
	message_put(reply,
		TAG_U32, stream->channel,	/* stream index/channel */
		TAG_U32, stream->channel,	/* source_output/stream index */
		TAG_INVALID);

	if (client->version >= 9) {
		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.fragsize,
			TAG_INVALID);
	}
	if (client->version >= 12) {
		message_put(reply,
			TAG_SAMPLE_SPEC, &stream->ss,
			TAG_CHANNEL_MAP, &stream->map,
			TAG_U32, impl->default_source.index,		/* source index */
			TAG_STRING, impl->default_source.name,		/* source name */
			TAG_BOOLEAN, false,		/* source suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		message_put(reply,
			TAG_USEC, 0ULL,			/* source configured latency */
			TAG_INVALID);
	}
	if (client->version >= 22) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(reply,
			TAG_FORMAT_INFO, &info,		/* source_output format */
			TAG_INVALID);
	}

	stream->create_tag = SPA_ID_INVALID;

	return send_message(client, reply);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct stream *stream = data;
	struct client *client = stream->client;

	switch (state) {
	case PW_STREAM_STATE_ERROR:
		reply_error(client, -1, ERR_INTERNAL);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		reply_error(client, -1, ERR_CONNECTIONTERMINATED);
		break;
	case PW_STREAM_STATE_CONNECTING:
		break;
	case PW_STREAM_STATE_PAUSED:
		break;
	case PW_STREAM_STATE_STREAMING:
		break;
	}
}

static const struct spa_pod *get_buffers_param(struct stream *s,
		struct buffer_attr *attr, struct spa_pod_builder *b)
{
	const struct spa_pod *param;
	uint32_t blocks, buffers, size, maxsize, stride;

	blocks = 1;
	stride = s->frame_size;

	if (s->direction == PW_DIRECTION_OUTPUT) {
		maxsize = attr->tlength;
		size = attr->minreq;
	} else {
		size = attr->fragsize;
		maxsize = attr->fragsize;
	}
	buffers = SPA_CLAMP(maxsize / size, MIN_BUFFERS, MAX_BUFFERS);

	pw_log_info("stream %p: stride %d maxsize %d size %u buffers %d", s, stride, maxsize,
			size, buffers);

	param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, MIN_BUFFERS, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								size, size, maxsize),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
	return param;
}

static int parse_param(const struct spa_pod *param, struct sample_spec *ss, struct channel_map *map)
{
	struct spa_audio_info info = { 0 };
//	uint32_t i;

        spa_format_parse(param, &info.media_type, &info.media_subtype);

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_audio_raw_parse(param, &info.info.raw) < 0 ||
	    !SPA_AUDIO_FORMAT_IS_INTERLEAVED(info.info.raw.format)) {
                return -ENOTSUP;
        }

        ss->format = format_id2pa(info.info.raw.format);
        if (ss->format == SAMPLE_INVALID)
                return -ENOTSUP;

        ss->rate = info.info.raw.rate;
        ss->channels = info.info.raw.channels;

	map->channels = info.info.raw.channels;
//	for (i = 0; i < map->channels; i++)
//		map->map[i] = info.info.raw.position[i];

	return 0;
}

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct stream *stream = data;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	int res;

	if (id != SPA_PARAM_Format || param == NULL)
		return;

	if ((res = parse_param(param, &stream->ss, &stream->map)) < 0) {
		pw_stream_set_error(stream->stream, res, "format not supported");
		return;
	}

	pw_log_info(NAME" %p: got rate:%u channels:%u", stream, stream->ss.rate, stream->ss.channels);

	stream->frame_size = sample_spec_frame_size(&stream->ss);

	if (stream->create_tag != SPA_ID_INVALID) {
		if (stream->volume_set) {
			pw_stream_set_control(stream->stream,
				SPA_PROP_channelVolumes, stream->volume.channels, stream->volume.values, 0);
		}
		if (stream->muted_set) {
			float val = stream->muted ? 1.0f : 0.0f;
			pw_stream_set_control(stream->stream,
				SPA_PROP_mute, 1, &val, 0);
		}
		if (stream->corked)
			pw_stream_set_active(stream->stream, false);

		if (stream->direction == PW_DIRECTION_OUTPUT) {
			reply_create_playback_stream(stream);
		} else {
			reply_create_record_stream(stream);
		}
	}

	params[n_params++] = get_buffers_param(stream, &stream->attr, &b);
	pw_stream_update_params(stream->stream, params, n_params);
}

static void update_timing_info(struct stream *stream)
{
	struct pw_time pwt;
	int64_t delay, pos;

	pw_stream_get_time(stream->stream, &pwt);

	stream->timestamp.tv_sec = pwt.now / SPA_NSEC_PER_SEC;
	stream->timestamp.tv_usec = (pwt.now % SPA_NSEC_PER_SEC) / SPA_NSEC_PER_USEC;

	if (pwt.rate.denom > 0) {
		uint64_t ticks = pwt.ticks;
		if (!stream->have_time)
                        stream->ticks_base = ticks;
                if (ticks > stream->ticks_base)
                        pos = ((ticks - stream->ticks_base) * stream->ss.rate / pwt.rate.denom) * stream->frame_size;
                else
                        pos = 0;
                delay = pwt.delay * SPA_USEC_PER_SEC / pwt.rate.denom;
                stream->have_time = true;
        } else {
                pos = delay = 0;
                stream->have_time = false;
        }
	if (stream->direction == PW_DIRECTION_OUTPUT)
		stream->read_index = pos;
	else
		stream->write_index = pos;
	stream->delay = delay;
}

static void stream_process_record(struct stream *stream)
{
	struct client *client = stream->client;
	struct pw_buffer *buffer;
	struct spa_buffer *buf;
	uint32_t size;
	struct message *msg;
	void *p;

	update_timing_info(stream);

	buffer = pw_stream_dequeue_buffer(stream->stream);
	if (buffer == NULL)
		return;

        buf = buffer->buffer;
        if ((p = buf->datas[0].data) == NULL)
		return;

	size = buf->datas[0].chunk->size;

	msg = message_alloc(client, size, stream->channel);
	if (msg != NULL) {
		memcpy(msg->data,
			SPA_MEMBER(p, buf->datas[0].chunk->offset, void),
			size);
		send_message(client, msg);
	}
	stream->write_index += size;
	pw_stream_queue_buffer(stream->stream, buffer);
}

static void stream_process_playback(struct stream *stream)
{
	struct client *client = stream->client;

	pw_log_trace(NAME" %p: process", stream);

	update_timing_info(stream);

	while (!spa_list_is_empty(&stream->messages)) {
		struct pw_buffer *buffer;
		struct spa_buffer *buf;
		struct message *msg;
		uint32_t size, maxsize;
		void *p;

		buffer = pw_stream_dequeue_buffer(stream->stream);
		if (buffer == NULL)
			break;

	        buf = buffer->buffer;
	        if ((p = buf->datas[0].data) == NULL)
			break;

		msg = spa_list_first(&stream->messages, struct message, link);
		maxsize = buf->datas[0].maxsize;
		size = SPA_MIN(msg->length - msg->offset, maxsize);
		memcpy(p, SPA_MEMBER(msg->data, msg->offset, void), size);

		pw_log_trace(NAME" %p: process message %p %d-%d/%d",
				stream, msg, msg->offset, size, msg->length);

		stream->read_index += size;
		stream->playing_for += size;
		msg->offset += size;
		if (msg->offset >= msg->length)
			message_free(client, msg, false);

	        buf->datas[0].chunk->offset = 0;
	        buf->datas[0].chunk->stride = stream->frame_size;
	        buf->datas[0].chunk->size = size;

		pw_stream_queue_buffer(stream->stream, buffer);
	}
	send_command_request(stream);
}

static void stream_process(void *data)
{
	struct stream *stream = data;
	if (stream->direction == PW_DIRECTION_OUTPUT)
		stream_process_playback(stream);
	else
		stream_process_record(stream);
}

static void stream_drained(void *data)
{
	struct stream *stream = data;
	pw_log_info(NAME" %p: drained channel:%u", stream, stream->channel);
	reply_simple_ack(stream->client, stream->drain_tag);
}

static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.process = stream_process,
	.drained = stream_drained,
};

#define MAXLENGTH		(4*1024*1024) /* 4MB */
#define DEFAULT_TLENGTH_MSEC	2000 /* 2s */
#define DEFAULT_PROCESS_MSEC	20   /* 20ms */
#define DEFAULT_FRAGSIZE_MSEC	DEFAULT_TLENGTH_MSEC

static uint32_t usec_to_bytes_round_up(uint64_t usec, const struct sample_spec *ss)
{
	uint64_t u;
	u = (uint64_t) usec * (uint64_t) ss->rate;
	u = (u + 1000000UL - 1) / 1000000UL;
	u *= sample_spec_frame_size(ss);
	return (uint32_t) u;
}

static void fix_playback_buffer_attr(struct stream *s, struct buffer_attr *attr)
{
	uint32_t frame_size, max_prebuf;

	frame_size = s->frame_size;

	if (attr->maxlength == (uint32_t) -1 || attr->maxlength > MAXLENGTH)
		attr->maxlength = MAXLENGTH;
	attr->maxlength -= attr->maxlength % frame_size;
	attr->maxlength = SPA_MAX(attr->maxlength, frame_size);

	if (attr->tlength == (uint32_t) -1)
		attr->tlength = usec_to_bytes_round_up(DEFAULT_TLENGTH_MSEC*1000, &s->ss);
	if (attr->tlength > attr->maxlength)
		attr->tlength = attr->maxlength;
	attr->tlength -= attr->tlength % frame_size;
	attr->tlength = SPA_MAX(attr->tlength, frame_size);

	if (attr->minreq == (uint32_t) -1) {
		uint32_t process = usec_to_bytes_round_up(DEFAULT_PROCESS_MSEC*1000, &s->ss);
		/* With low-latency, tlength/4 gives a decent default in all of traditional,
		 * adjust latency and early request modes. */
		uint32_t m = attr->tlength / 4;
		m -= m % frame_size;
		attr->minreq = SPA_MIN(process, m);
	}

	if (attr->tlength < attr->minreq+frame_size)
		attr->tlength = attr->minreq + frame_size;

	attr->minreq -= attr->minreq % frame_size;
	if (attr->minreq <= 0) {
		attr->minreq = frame_size;
		attr->tlength += frame_size*2;
	}
	if (attr->tlength <= attr->minreq)
		attr->tlength = attr->minreq*2 + frame_size;

	max_prebuf = attr->tlength + frame_size - attr->minreq;
	if (attr->prebuf == (uint32_t) -1 || attr->prebuf > max_prebuf)
		attr->prebuf = max_prebuf;
	attr->prebuf -= attr->prebuf % frame_size;
	attr->prebuf = SPA_MAX(attr->prebuf, frame_size);
}

static int do_create_playback_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	int res;
	struct sample_spec ss;
	struct channel_map map;
	uint32_t sink_index, syncid;
	const char *sink_name;
	struct buffer_attr attr;
	bool corked = false,
		no_remap = false,
		no_remix = false,
		fix_format = false,
		fix_rate = false,
		fix_channels = false,
		no_move = false,
		variable_rate = false,
		muted = false,
		adjust_latency = false,
		early_requests = false,
		dont_inhibit_auto_suspend = false,
		volume_set = true,
		muted_set = false,
		fail_on_suspend = false,
		relative_volume = false,
		passthrough = false;
	struct cvolume volume;
	struct pw_properties *props = NULL;
	uint8_t n_formats = 0;
	struct format_info *formats = NULL;
	struct stream *stream;
        struct spa_audio_info_raw info;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	props = pw_properties_new(NULL, NULL);

	if (client->version < 13) {
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			goto error;
		if (name == NULL) {
			res = -EPROTO;
			goto error;
		}
	}
	if ((res = message_get(m,
			TAG_SAMPLE_SPEC, &ss,
			TAG_CHANNEL_MAP, &map,
			TAG_U32, &sink_index,
			TAG_STRING, &sink_name,
			TAG_U32, &attr.maxlength,
			TAG_BOOLEAN, &corked,
			TAG_U32, &attr.tlength,
			TAG_U32, &attr.prebuf,
			TAG_U32, &attr.minreq,
			TAG_U32, &syncid,
			TAG_CVOLUME, &volume,
			TAG_INVALID)) < 0)
		goto error;

	pw_log_info(NAME" %p: CREATE_PLAYBACK_STREAM corked:%u sink-name:%s sink-idx:%u",
			impl, corked, sink_name, sink_index);

	if (client->version >= 12) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &no_remap,
				TAG_BOOLEAN, &no_remix,
				TAG_BOOLEAN, &fix_format,
				TAG_BOOLEAN, &fix_rate,
				TAG_BOOLEAN, &fix_channels,
				TAG_BOOLEAN, &no_move,
				TAG_BOOLEAN, &variable_rate,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 13) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 14) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 15) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 17) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &relative_volume,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 18) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 21) {
		if ((res = message_get(m,
				TAG_U8, &n_formats,
				TAG_INVALID)) < 0)
			goto error;

		if (n_formats) {
			uint8_t i;
			formats = calloc(n_formats, sizeof(struct format_info));
			for (i = 0; i < n_formats; i++) {
				if ((res = message_get(m,
						TAG_FORMAT_INFO, &formats[i],
						TAG_INVALID)) < 0)
					goto error;
			}
		}
	}
	if (m->offset != m->length) {
		res = -EPROTO;
		goto error;
	}

	stream = calloc(1, sizeof(struct stream));
	if (stream == NULL) {
		res = -errno;
		goto error;
	}
	stream->impl = impl;
	stream->client = client;
	stream->corked = corked;
	stream->adjust_latency = adjust_latency;
	stream->channel = pw_map_insert_new(&client->streams, stream);
	spa_list_init(&stream->messages);

	stream->direction = PW_DIRECTION_OUTPUT;
	stream->create_tag = tag;
	stream->ss = ss;
	stream->map = map;
	stream->volume = volume;
	stream->volume_set = volume_set;
	stream->muted = muted;
	stream->muted_set = muted_set;

	stream->frame_size = sample_spec_frame_size(&stream->ss);

	fix_playback_buffer_attr(stream, &attr);
	stream->attr = attr;

	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
			stream->attr.minreq * 2 / stream->frame_size, ss.rate);

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL) {
		res = -errno;
		goto error;
	}
	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

        info = SPA_AUDIO_INFO_RAW_INIT(
			.format = format_pa2id(ss.format),
			.channels = ss.channels,
			.rate = ss.rate);
	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	pw_stream_connect(stream->stream,
			PW_DIRECTION_OUTPUT,
			SPA_ID_INVALID,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, n_params);

	return 0;

error:
	if (props)
		pw_properties_free(props);
	if (stream)
		stream_free(stream);
	return res;
}

static void fix_record_buffer_attr(struct stream *s, struct buffer_attr *attr)
{
	uint32_t frame_size;

	frame_size = s->frame_size;

	if (attr->maxlength == (uint32_t) -1 || attr->maxlength > MAXLENGTH)
		attr->maxlength = MAXLENGTH;
	attr->maxlength -= attr->maxlength % frame_size;
	attr->maxlength = SPA_MAX(attr->maxlength, frame_size);

	if (attr->fragsize == (uint32_t) -1 || attr->fragsize == 0)
		attr->fragsize = usec_to_bytes_round_up(DEFAULT_FRAGSIZE_MSEC*1000, &s->ss);
	attr->fragsize -= attr->fragsize % frame_size;
	attr->fragsize = SPA_MAX(attr->fragsize, frame_size);
	if (attr->fragsize > attr->maxlength)
		attr->fragsize = attr->maxlength;

	pw_log_info(NAME" %p: maxlength:%u fragsize:%u", s,
			attr->maxlength, attr->fragsize);
}

static int do_create_record_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	int res;
	struct sample_spec ss;
	struct channel_map map;
	uint32_t source_index;
	const char *source_name;
	struct buffer_attr attr;
	bool corked = false,
		no_remap = false,
		no_remix = false,
		fix_format = false,
		fix_rate = false,
		fix_channels = false,
		no_move = false,
		variable_rate = false,
		peak_detect = false,
		adjust_latency = false,
		early_requests = false,
		dont_inhibit_auto_suspend = false,
		volume_set = true,
		muted = false,
		muted_set = false,
		fail_on_suspend = false,
		relative_volume = false,
		passthrough = false;
	uint32_t direct_on_input_idx;
	struct cvolume volume;
	struct pw_properties *props = NULL;
	uint8_t n_formats = 0;
	struct format_info *formats = NULL;
	struct stream *stream;
        struct spa_audio_info_raw info;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	props = pw_properties_new(NULL, NULL);

	if (client->version < 13) {
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			goto error;
		if (name == NULL) {
			res = -EPROTO;
			goto error;
		}
	}
	if ((res = message_get(m,
			TAG_SAMPLE_SPEC, &ss,
			TAG_CHANNEL_MAP, &map,
			TAG_U32, &source_index,
			TAG_STRING, &source_name,
			TAG_U32, &attr.maxlength,
			TAG_BOOLEAN, &corked,
			TAG_U32, &attr.fragsize,
			TAG_INVALID)) < 0)
		goto error;

	pw_log_info(NAME" %p: CREATE_RECORD_STREAM corked:%u source-name:%s source-index:%u",
			impl, corked, source_name, source_index);

	if (client->version >= 12) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &no_remap,
				TAG_BOOLEAN, &no_remix,
				TAG_BOOLEAN, &fix_format,
				TAG_BOOLEAN, &fix_rate,
				TAG_BOOLEAN, &fix_channels,
				TAG_BOOLEAN, &no_move,
				TAG_BOOLEAN, &variable_rate,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 13) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &peak_detect,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_U32, &direct_on_input_idx,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 14) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 15) {
		if ((res = message_get(m,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 22) {
		if ((res = message_get(m,
				TAG_U8, &n_formats,
				TAG_INVALID)) < 0)
			goto error;

		if (n_formats) {
			uint8_t i;
			formats = calloc(n_formats, sizeof(struct format_info));
			for (i = 0; i < n_formats; i++) {
				if ((res = message_get(m,
						TAG_FORMAT_INFO, &formats[i],
						TAG_INVALID)) < 0)
					goto error;
			}
		}
		if ((res = message_get(m,
				TAG_CVOLUME, &volume,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &relative_volume,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (m->offset != m->length) {
		res = -EPROTO;
		goto error;
	}

	stream = calloc(1, sizeof(struct stream));
	if (stream == NULL) {
		res = -errno;
		goto error;
	}
	stream->direction = PW_DIRECTION_INPUT;
	stream->impl = impl;
	stream->client = client;
	stream->corked = corked;
	stream->adjust_latency = adjust_latency;
	stream->channel = pw_map_insert_new(&client->streams, stream);
	spa_list_init(&stream->messages);
	stream->create_tag = tag;
	stream->ss = ss;
	stream->map = map;

	stream->frame_size = sample_spec_frame_size(&stream->ss);

	fix_record_buffer_attr(stream, &attr);
	stream->attr = attr;

	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
			stream->attr.fragsize / stream->frame_size, ss.rate);

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL) {
		res = -errno;
		goto error;
	}
	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

        info = SPA_AUDIO_INFO_RAW_INIT(
			.format = format_pa2id(ss.format),
			.channels = ss.channels,
			.rate = ss.rate);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	pw_stream_connect(stream->stream,
			PW_DIRECTION_INPUT,
			SPA_ID_INVALID,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS,
			params, n_params);

	return 0;

error:
	if (props)
		pw_properties_free(props);
	if (stream)
		stream_free(stream);
	return res;
}

static int do_delete_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DELETE_STREAM channel:%u", impl, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	stream_free(stream);

	return reply_simple_ack(client, tag);
}

static int do_get_playback_latency(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t channel;
	struct timeval tv;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_TIMEVAL, &tv,
			TAG_INVALID)) < 0)
		return res;

	pw_log_debug(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;


	pw_log_debug("read:%"PRIi64" write:%"PRIi64" queued:%"PRIi64" delay:%"PRIi64,
			stream->read_index, stream->write_index,
			stream->write_index - stream->read_index, stream->delay);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_USEC, stream->delay,	/* sink latency + queued samples */
		TAG_USEC, 0,			/* always 0 */
		TAG_BOOLEAN, stream->playing_for > 0 &&
				!stream->corked,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &stream->timestamp,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(reply,
			TAG_U64, stream->underrun_for,
			TAG_U64, stream->playing_for,
			TAG_INVALID);
	}
	return send_message(client, reply);
}

static int do_get_record_latency(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t channel;
	struct timeval tv, now;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_TIMEVAL, &tv,
			TAG_INVALID)) < 0)
		return res;

	pw_log_debug(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	gettimeofday(&now, NULL);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_USEC, 0,			/* monitor latency */
		TAG_USEC, stream->delay,	/* source latency + queued */
		TAG_BOOLEAN, !stream->corked,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &stream->timestamp,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	return send_message(client, reply);
}

static int do_cork_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	bool cork;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_BOOLEAN, &cork,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u cork:%s",
			impl, commands[command].name, channel, cork ? "yes" : "no");

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	pw_stream_set_active(stream->stream, !cork);
	stream->corked = cork;
	stream->playing_for = 0;

	return reply_simple_ack(client, tag);
}

static int do_flush_trigger_prebuf_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u",
			impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	switch (command) {
	case COMMAND_FLUSH_PLAYBACK_STREAM:
	case COMMAND_FLUSH_RECORD_STREAM:
		stream_flush(stream);
		pw_stream_flush(stream->stream, false);
		send_command_request(stream);
		break;
	case COMMAND_TRIGGER_PLAYBACK_STREAM:
	case COMMAND_PREBUF_PLAYBACK_STREAM:
		break;
	default:
		return -EINVAL;
	}

	return reply_simple_ack(client, tag);
}

static int do_error_access(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return reply_error(client, tag, ERR_ACCESS);
}

static int do_set_stream_volume(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;
	struct cvolume volume;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_CVOLUME, &volume,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DO_STREAM_VOLUME channel:%u",
			impl, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	stream->volume = volume;
	stream->volume_set = true;

	pw_stream_set_control(stream->stream,
			SPA_PROP_channelVolumes, volume.channels, volume.values,
			0);

	return reply_simple_ack(client, tag);
}

static int do_set_stream_mute(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;
	bool mute;
	float val;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_BOOLEAN, &mute,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DO_SET_STREAM_MUTE channel:%u mute:%u",
			impl, channel, mute);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	stream->muted = mute;
	stream->muted_set = true;

	val = mute ? 1.0f : 0.0f;
	pw_stream_set_control(stream->stream,
			SPA_PROP_mute, 1, &val,
			0);

	return reply_simple_ack(client, tag);
}

static int do_set_stream_name(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	const char *name = NULL;
	struct spa_dict_item items[1];
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return res;

	if (name == NULL)
		return -EINVAL;

	pw_log_info(NAME" %p: SET_STREAM_NAME channel:%d name:%s", impl, channel, name);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, name);
	pw_stream_update_properties(stream->stream,
			&SPA_DICT_INIT(items, 1));

	return reply_simple_ack(client, tag);
}

static int do_update_proplist(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel, mode;
	struct stream *stream;
	struct pw_properties *props;
	int res;

	props = pw_properties_new(NULL, NULL);

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		if ((res = message_get(m,
				TAG_U32, &channel,
				TAG_INVALID)) < 0)
			goto exit;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info(NAME" %p: %s channel:%d", impl, commands[command].name, channel);

	if ((res = message_get(m,
			TAG_U32, &mode,
			TAG_PROPLIST, &props,
			TAG_INVALID)) < 0)
		goto exit;

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		stream = pw_map_lookup(&client->streams, channel);
		if (stream == NULL) {
			res = -EINVAL;
			goto exit;
		}
		pw_stream_update_properties(stream->stream, &props->dict);
	} else {
		pw_core_update_properties(client->core, &props->dict);
	}
	res = reply_simple_ack(client, tag);
exit:
	pw_properties_free(props);
	return res;
}

static int do_remove_proplist(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t i, channel;
	struct stream *stream;
	struct pw_properties *props;
	struct spa_dict dict;
	struct spa_dict_item *items;
	int res;

	props = pw_properties_new(NULL, NULL);

	if (command != COMMAND_REMOVE_CLIENT_PROPLIST) {
		if ((res = message_get(m,
				TAG_U32, &channel,
				TAG_INVALID)) < 0)
			goto exit;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info(NAME" %p: %s channel:%d", impl, commands[command].name, channel);

	while (true) {
		const char *key;

		if ((res = message_get(m,
				TAG_STRING, &key,
				TAG_INVALID)) < 0)
			goto exit;
		if (key == NULL)
			break;
		pw_properties_set(props, key, key);
	}

	items = alloca(sizeof(struct spa_dict_item) * dict.n_items);
	dict.n_items = props->dict.n_items;
	dict.items = items;
	for (i = 0; i < dict.n_items; i++) {
		items[i].key = props->dict.items[i].key;
		items[i].value = NULL;
	}

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		stream = pw_map_lookup(&client->streams, channel);
		if (stream == NULL) {
			res = -EINVAL;
			goto exit;
		}
		pw_stream_update_properties(stream->stream, &dict);
	} else {
		pw_core_update_properties(client->core, &dict);
	}
	res = reply_simple_ack(client, tag);
exit:
	pw_properties_free(props);
	return res;
}

static int do_get_server_info(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	char name[256];
	struct message *reply;
	struct sample_spec ss;
	struct channel_map map;

	pw_log_info(NAME" %p: GET_SERVER_INFO", impl);

	snprintf(name, sizeof(name)-1, "PulseAudio (on PipeWire %s)", pw_get_library_version());

	spa_zero(ss);
	ss.format = SAMPLE_FLOAT32LE;
	ss.rate = 44100;
	ss.channels = 2;

	spa_zero(map);
	map.channels = 2;
	map.map[0] = 1;
	map.map[1] = 2;

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_STRING, name,
		TAG_STRING, "14.0.0",
		TAG_STRING, pw_get_user_name(),
		TAG_STRING, pw_get_host_name(),
		TAG_SAMPLE_SPEC, &ss,
		TAG_STRING, impl->default_sink.name,
		TAG_STRING, impl->default_source.name,
		TAG_U32, 0,
		TAG_INVALID);

	if (client->version >= 15) {
		message_put(reply,
			TAG_CHANNEL_MAP, &map,
			TAG_INVALID);
	}
	return send_message(client, reply);
}

static int do_stat(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;

	pw_log_info(NAME" %p: STAT", impl);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_U32, 0,	/* n_allocated */
		TAG_U32, 0,	/* allocated size */
		TAG_U32, 0,	/* n_accumulated */
		TAG_U32, 0,	/* accumulated_size */
		TAG_U32, 0,	/* sample cache size */
		TAG_INVALID);

	return send_message(client, reply);
}

static int do_lookup(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	const char *name = NULL;
	struct message *reply;
	uint32_t idx = 0;
	int res;

	if ((res = message_get(m,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return res;
	if (name == NULL)
		return -EINVAL;

	pw_log_info(NAME" %p: LOOKUP %s", impl, name);

	reply = reply_new(client, tag);
	message_put(reply,
		TAG_U32, idx,
		TAG_INVALID);

	return send_message(client, reply);
}

static int do_drain_stream(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DRAIN channel:%d", impl, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL) {
		res = -EINVAL;
		return res;
	}

	pw_stream_flush(stream->stream, true);

	return reply_simple_ack(client, tag);
}

static void fill_client_info(struct client *client, struct message *m)
{
	message_put(m,
		TAG_U32, 0,				/* client index */
		TAG_STRING, pw_properties_get(client->props, "application.name"),
		TAG_U32, SPA_ID_INVALID,		/* module */ 
		TAG_STRING, "PipeWire",			/* driver */
		TAG_INVALID);
	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, client->props,
			TAG_INVALID);
	}
}

static void fill_sink_info(struct client *client, struct message *m, struct device *sink)
{
	message_put(m,
		TAG_U32, sink->index,		/* sink index */
		TAG_STRING, sink->name,
		TAG_STRING, pw_properties_get(sink->props, "device.description"),
		TAG_SAMPLE_SPEC, &sink->ss,
		TAG_CHANNEL_MAP, &sink->map,
		TAG_U32, SPA_ID_INVALID,	/* module index */
		TAG_CVOLUME, &sink->volume,
		TAG_BOOLEAN, sink->muted,
		TAG_U32, SPA_ID_INVALID,	/* monitor source */
		TAG_STRING, NULL,		/* monitor source name */
		TAG_USEC, 0LL,			/* latency */
		TAG_STRING, "PipeWire",		/* driver */
		TAG_U32, 0,			/* flags */
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, sink->props,
			TAG_USEC, 0LL,			/* requested latency */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_VOLUME, 1.0f,		/* base volume */
			TAG_U32, 0,			/* state */
			TAG_U32, 256,			/* n_volume_steps */
			TAG_U32, SPA_ID_INVALID,	/* card index */
			TAG_INVALID);
	}
	if (client->version >= 16) {
		message_put(m,
			TAG_U32, 0,			/* n_ports */
			TAG_INVALID);
		message_put(m,
			TAG_STRING, NULL,		/* active port name */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(m,
			TAG_U8, 1,			/* n_formats */
			TAG_FORMAT_INFO, &info,
			TAG_INVALID);
	}
}

static void fill_source_info(struct client *client, struct message *m, struct device *source)
{
	message_put(m,
		TAG_U32, source->index,		/* source index */
		TAG_STRING, source->name,
		TAG_STRING, pw_properties_get(source->props, "device.description"),
		TAG_SAMPLE_SPEC, &source->ss,
		TAG_CHANNEL_MAP, &source->map,
		TAG_U32, SPA_ID_INVALID,	/* module index */
		TAG_CVOLUME, &source->volume,
		TAG_BOOLEAN, source->muted,
		TAG_U32, SPA_ID_INVALID,	/* monitor source */
		TAG_STRING, NULL,		/* monitor source name */
		TAG_USEC, 0LL,			/* latency */
		TAG_STRING, "PipeWire",		/* driver */
		TAG_U32, 0,			/* flags */
		TAG_INVALID);

	if (client->version >= 13) {
		message_put(m,
			TAG_PROPLIST, source->props,
			TAG_USEC, 0LL,			/* requested latency */
			TAG_INVALID);
	}
	if (client->version >= 15) {
		message_put(m,
			TAG_VOLUME, 1.0f,		/* base volume */
			TAG_U32, 0,			/* state */
			TAG_U32, 256,			/* n_volume_steps */
			TAG_U32, SPA_ID_INVALID,	/* card index */
			TAG_INVALID);
	}
	if (client->version >= 16) {
		message_put(m,
			TAG_U32, 0,			/* n_ports */
			TAG_INVALID);
		message_put(m,
			TAG_STRING, NULL,		/* active port name */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		message_put(m,
			TAG_U8, 1,			/* n_formats */
			TAG_FORMAT_INFO, &info,
			TAG_INVALID);
	}
}

static int do_get_info(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;
	uint32_t idx;
	const char *name = NULL;
	int res;

	if ((res = message_get(m,
			TAG_U32, &idx,
			TAG_INVALID)) < 0)
		return res;

	switch (command) {
	case COMMAND_GET_SINK_INFO:
	case COMMAND_GET_SOURCE_INFO:
	case COMMAND_GET_CARD_INFO:
	case COMMAND_GET_SAMPLE_INFO:
		if ((res = message_get(m,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			return res;
		break;
	}

	pw_log_info(NAME" %p: %s idx:%u name:%s", impl,
			commands[command].name, idx, name);

	reply = reply_new(client, tag);
	switch (command) {
	case COMMAND_GET_CLIENT_INFO:
		fill_client_info(client, reply);
		break;
	case COMMAND_GET_MODULE_INFO:
	case COMMAND_GET_CARD_INFO:
	case COMMAND_GET_SAMPLE_INFO:
		return reply_error(client, -1, ERR_NOENTITY);
	case COMMAND_GET_SINK_INFO:
		fill_sink_info(client, reply, &impl->default_sink);
		break;
	case COMMAND_GET_SOURCE_INFO:
		fill_source_info(client, reply, &impl->default_source);
		break;
	case COMMAND_GET_SINK_INPUT_INFO:
	case COMMAND_GET_SOURCE_OUTPUT_INFO:
		/* fixme add ourselves */
		break;
	default:
		return -ENOTSUP;
	}
	return send_message(client, reply);
}

static int do_get_info_list(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	struct message *reply;

	pw_log_info(NAME" %p: %s", impl, commands[command].name);

	reply = reply_new(client, tag);
	switch (command) {
	case COMMAND_GET_CLIENT_INFO_LIST:
		fill_client_info(client, reply);
		break;
	case COMMAND_GET_MODULE_INFO_LIST:
	case COMMAND_GET_CARD_INFO_LIST:
	case COMMAND_GET_SAMPLE_INFO_LIST:
		break;
	case COMMAND_GET_SINK_INFO_LIST:
		fill_sink_info(client, reply, &impl->default_sink);
		break;
	case COMMAND_GET_SOURCE_INFO_LIST:
		fill_source_info(client, reply, &impl->default_source);
		break;
	case COMMAND_GET_SINK_INPUT_INFO_LIST:
	case COMMAND_GET_SOURCE_OUTPUT_INFO_LIST:
		/* fixme add ourselves */
		break;
	default:
		return -ENOTSUP;
	}
	return send_message(client, reply);
}

static int do_set_stream_buffer_attr(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	struct message *reply;
	struct buffer_attr attr;
	int res;
	bool adjust_latency = false, early_requests = false;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL) {
		res = -EINVAL;
		return res;
	}

	reply = reply_new(client, tag);

	if (command == COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR) {
		if ((res = message_get(m,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.tlength,
				TAG_U32, &attr.prebuf,
				TAG_U32, &attr.minreq,
				TAG_INVALID)) < 0)
			return res;
		if (client->version >= 13) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &adjust_latency,
					TAG_INVALID)) < 0)
				return res;
		}
		if (client->version >= 14) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &early_requests,
					TAG_INVALID)) < 0)
				return res;
		}

		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_INVALID);
		if (client->version >= 13) {
			message_put(reply,
				TAG_USEC, 0,		/* configured_sink_latency */
				TAG_INVALID);
		}
	} else {
		if ((res = message_get(m,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.fragsize,
				TAG_INVALID)) < 0)
			return res;
		if (client->version >= 13) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &adjust_latency,
					TAG_INVALID)) < 0)
				return res;
		}
		if (client->version >= 14) {
			if ((res = message_get(m,
					TAG_BOOLEAN, &early_requests,
					TAG_INVALID)) < 0)
				return res;
		}
		message_put(reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.fragsize,
			TAG_INVALID);
		if (client->version >= 13) {
			message_put(reply,
				TAG_USEC, 0,		/* configured_source_latency */
				TAG_INVALID);
		}
	}
	return send_message(client, reply);
}

static int do_update_stream_sample_rate(struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct impl *impl = client->impl;
	uint32_t channel, rate;
	struct stream *stream;
	int res;

	if ((res = message_get(m,
			TAG_U32, &channel,
			TAG_U32, &rate,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u rate:%u", impl, commands[command].name, channel, rate);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL) {
		res = -EINVAL;
		return res;
	}
	return reply_simple_ack(client, tag);
}

static const struct command commands[COMMAND_MAX] =
{
	[COMMAND_ERROR] = { "ERROR", },
	[COMMAND_TIMEOUT] = { "TIMEOUT", }, /* pseudo command */
	[COMMAND_REPLY] = { "REPLY", },

	/* CLIENT->SERVER */
	[COMMAND_CREATE_PLAYBACK_STREAM] = { "CREATE_PLAYBACK_STREAM", do_create_playback_stream, },
	[COMMAND_DELETE_PLAYBACK_STREAM] = { "DELETE_PLAYBACK_STREAM", do_delete_stream, },
	[COMMAND_CREATE_RECORD_STREAM] = { "CREATE_RECORD_STREAM", do_create_record_stream, },
	[COMMAND_DELETE_RECORD_STREAM] = { "DELETE_RECORD_STREAM", do_delete_stream, },
	[COMMAND_EXIT] = { "EXIT", do_error_access },
	[COMMAND_AUTH] = { "AUTH", do_command_auth, },
	[COMMAND_SET_CLIENT_NAME] = { "SET_CLIENT_NAME", do_set_client_name, },
	[COMMAND_LOOKUP_SINK] = { "LOOKUP_SINK", do_lookup, },
	[COMMAND_LOOKUP_SOURCE] = { "LOOKUP_SOURCE", do_lookup, },
	[COMMAND_DRAIN_PLAYBACK_STREAM] = { "DRAIN_PLAYBACK_STREAM", do_drain_stream, },
	[COMMAND_STAT] = { "STAT", do_stat, },
	[COMMAND_GET_PLAYBACK_LATENCY] = { "GET_PLAYBACK_LATENCY", do_get_playback_latency, },
	[COMMAND_CREATE_UPLOAD_STREAM] = { "CREATE_UPLOAD_STREAM", do_error_access, },
	[COMMAND_DELETE_UPLOAD_STREAM] = { "DELETE_UPLOAD_STREAM", do_error_access, },
	[COMMAND_FINISH_UPLOAD_STREAM] = { "FINISH_UPLOAD_STREAM", do_error_access, },
	[COMMAND_PLAY_SAMPLE] = { "PLAY_SAMPLE", do_error_access, },
	[COMMAND_REMOVE_SAMPLE] = { "REMOVE_SAMPLE", do_error_access, },

	[COMMAND_GET_SERVER_INFO] = { "GET_SERVER_INFO", do_get_server_info },
	[COMMAND_GET_SINK_INFO] = { "GET_SINK_INFO", do_get_info, },
	[COMMAND_GET_SOURCE_INFO] = { "GET_SOURCE_INFO", do_get_info, },
	[COMMAND_GET_MODULE_INFO] = { "GET_MODULE_INFO", do_get_info, },
	[COMMAND_GET_CLIENT_INFO] = { "GET_CLIENT_INFO", do_get_info, },
	[COMMAND_GET_SINK_INPUT_INFO] = { "GET_SINK_INPUT_INFO", do_get_info, },
	[COMMAND_GET_SOURCE_OUTPUT_INFO] = { "GET_SOURCE_OUTPUT_INFO", do_get_info, },
	[COMMAND_GET_SAMPLE_INFO] = { "GET_SAMPLE_INFO", do_get_info, },
	[COMMAND_GET_CARD_INFO] = { "GET_CARD_INFO", do_get_info, },
	[COMMAND_SUBSCRIBE] = { "SUBSCRIBE", do_subscribe, },

	[COMMAND_GET_SINK_INFO_LIST] = { "GET_SINK_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SOURCE_INFO_LIST] = { "GET_SOURCE_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_MODULE_INFO_LIST] = { "GET_MODULE_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_CLIENT_INFO_LIST] = { "GET_CLIENT_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SINK_INPUT_INFO_LIST] = { "GET_SINK_INPUT_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SOURCE_OUTPUT_INFO_LIST] = { "GET_SOURCE_OUTPUT_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_SAMPLE_INFO_LIST] = { "GET_SAMPLE_INFO_LIST", do_get_info_list, },
	[COMMAND_GET_CARD_INFO_LIST] = { "GET_CARD_INFO_LIST", do_get_info_list, },

	[COMMAND_SET_SINK_VOLUME] = { "SET_SINK_VOLUME", do_error_access, },
	[COMMAND_SET_SINK_INPUT_VOLUME] = { "SET_SINK_INPUT_VOLUME", do_set_stream_volume, },
	[COMMAND_SET_SOURCE_VOLUME] = { "SET_SOURCE_VOLUME", do_error_access, },

	[COMMAND_SET_SINK_MUTE] = { "SET_SINK_MUTE", do_error_access, },
	[COMMAND_SET_SOURCE_MUTE] = { "SET_SOURCE_MUTE", do_error_access, },

	[COMMAND_CORK_PLAYBACK_STREAM] = { "CORK_PLAYBACK_STREAM", do_cork_stream, },
	[COMMAND_FLUSH_PLAYBACK_STREAM] = { "FLUSH_PLAYBACK_STREAM", do_flush_trigger_prebuf_stream, },
	[COMMAND_TRIGGER_PLAYBACK_STREAM] = { "TRIGGER_PLAYBACK_STREAM", do_flush_trigger_prebuf_stream, },
	[COMMAND_PREBUF_PLAYBACK_STREAM] = { "PREBUF_PLAYBACK_STREAM", do_flush_trigger_prebuf_stream, },

	[COMMAND_SET_DEFAULT_SINK] = { "SET_DEFAULT_SINK", do_error_access, },
	[COMMAND_SET_DEFAULT_SOURCE] = { "SET_DEFAULT_SOURCE", do_error_access, },

	[COMMAND_SET_PLAYBACK_STREAM_NAME] = { "SET_PLAYBACK_STREAM_NAME", do_set_stream_name, },
	[COMMAND_SET_RECORD_STREAM_NAME] = { "SET_RECORD_STREAM_NAME", do_set_stream_name, },

	[COMMAND_KILL_CLIENT] = { "KILL_CLIENT", do_error_access, },
	[COMMAND_KILL_SINK_INPUT] = { "KILL_SINK_INPUT", do_error_access, },
	[COMMAND_KILL_SOURCE_OUTPUT] = { "KILL_SOURCE_OUTPUT", do_error_access, },

	[COMMAND_LOAD_MODULE] = { "LOAD_MODULE", do_error_access, },
	[COMMAND_UNLOAD_MODULE] = { "UNLOAD_MODULE", do_error_access, },

	/* Obsolete */
	[COMMAND_ADD_AUTOLOAD___OBSOLETE] = { "ADD_AUTOLOAD___OBSOLETE", do_error_access, },
	[COMMAND_REMOVE_AUTOLOAD___OBSOLETE] = { "REMOVE_AUTOLOAD___OBSOLETE", do_error_access, },
	[COMMAND_GET_AUTOLOAD_INFO___OBSOLETE] = { "GET_AUTOLOAD_INFO___OBSOLETE", do_error_access, },
	[COMMAND_GET_AUTOLOAD_INFO_LIST___OBSOLETE] = { "GET_AUTOLOAD_INFO_LIST___OBSOLETE", do_error_access, },

	[COMMAND_GET_RECORD_LATENCY] = { "GET_RECORD_LATENCY", do_get_record_latency, },
	[COMMAND_CORK_RECORD_STREAM] = { "CORK_RECORD_STREAM", do_cork_stream, },
	[COMMAND_FLUSH_RECORD_STREAM] = { "FLUSH_RECORD_STREAM", do_flush_trigger_prebuf_stream, },

	/* SERVER->CLIENT */
	[COMMAND_REQUEST] = { "REQUEST", },
	[COMMAND_OVERFLOW] = { "OVERFLOW", },
	[COMMAND_UNDERFLOW] = { "UNDERFLOW", },
	[COMMAND_PLAYBACK_STREAM_KILLED] = { "PLAYBACK_STREAM_KILLED", },
	[COMMAND_RECORD_STREAM_KILLED] = { "RECORD_STREAM_KILLED", },
	[COMMAND_SUBSCRIBE_EVENT] = { "SUBSCRIBE_EVENT", },

	/* A few more client->server commands */

	/* Supported since protocol v10 (0.9.5) */
	[COMMAND_MOVE_SINK_INPUT] = { "MOVE_SINK_INPUT", do_error_access, },
	[COMMAND_MOVE_SOURCE_OUTPUT] = { "MOVE_SOURCE_OUTPUT", do_error_access, },

	/* Supported since protocol v11 (0.9.7) */
	[COMMAND_SET_SINK_INPUT_MUTE] = { "SET_SINK_INPUT_MUTE", do_set_stream_mute, },

	[COMMAND_SUSPEND_SINK] = { "SUSPEND_SINK", do_error_access, },
	[COMMAND_SUSPEND_SOURCE] = { "SUSPEND_SOURCE", do_error_access, },

	/* Supported since protocol v12 (0.9.8) */
	[COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR] = { "SET_PLAYBACK_STREAM_BUFFER_ATTR", do_set_stream_buffer_attr, },
	[COMMAND_SET_RECORD_STREAM_BUFFER_ATTR] = { "SET_RECORD_STREAM_BUFFER_ATTR", do_set_stream_buffer_attr, },

	[COMMAND_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE] = { "UPDATE_PLAYBACK_STREAM_SAMPLE_RATE", do_update_stream_sample_rate, },
	[COMMAND_UPDATE_RECORD_STREAM_SAMPLE_RATE] = { "UPDATE_RECORD_STREAM_SAMPLE_RATE", do_update_stream_sample_rate, },

	/* SERVER->CLIENT */
	[COMMAND_PLAYBACK_STREAM_SUSPENDED] = { "PLAYBACK_STREAM_SUSPENDED", },
	[COMMAND_RECORD_STREAM_SUSPENDED] = { "RECORD_STREAM_SUSPENDED", },
	[COMMAND_PLAYBACK_STREAM_MOVED] = { "PLAYBACK_STREAM_MOVED", },
	[COMMAND_RECORD_STREAM_MOVED] = { "RECORD_STREAM_MOVED", },

	/* Supported since protocol v13 (0.9.11) */
	[COMMAND_UPDATE_RECORD_STREAM_PROPLIST] = { "UPDATE_RECORD_STREAM_PROPLIST", do_update_proplist, },
	[COMMAND_UPDATE_PLAYBACK_STREAM_PROPLIST] = { "UPDATE_PLAYBACK_STREAM_PROPLIST", do_update_proplist, },
	[COMMAND_UPDATE_CLIENT_PROPLIST] = { "UPDATE_CLIENT_PROPLIST", do_update_proplist, },

	[COMMAND_REMOVE_RECORD_STREAM_PROPLIST] = { "REMOVE_RECORD_STREAM_PROPLIST", do_remove_proplist, },
	[COMMAND_REMOVE_PLAYBACK_STREAM_PROPLIST] = { "REMOVE_PLAYBACK_STREAM_PROPLIST", do_remove_proplist, },
	[COMMAND_REMOVE_CLIENT_PROPLIST] = { "REMOVE_CLIENT_PROPLIST", do_remove_proplist, },

	/* SERVER->CLIENT */
	[COMMAND_STARTED] = { "STARTED", },

	/* Supported since protocol v14 (0.9.12) */
	[COMMAND_EXTENSION] = { "EXTENSION", do_error_access, },
	/* Supported since protocol v15 (0.9.15) */
	[COMMAND_SET_CARD_PROFILE] = { "SET_CARD_PROFILE", do_error_access, },

	/* SERVER->CLIENT */
	[COMMAND_CLIENT_EVENT] = { "CLIENT_EVENT", },
	[COMMAND_PLAYBACK_STREAM_EVENT] = { "PLAYBACK_STREAM_EVENT", },
	[COMMAND_RECORD_STREAM_EVENT] = { "RECORD_STREAM_EVENT", },

	/* SERVER->CLIENT */
	[COMMAND_PLAYBACK_BUFFER_ATTR_CHANGED] = { "PLAYBACK_BUFFER_ATTR_CHANGED", },
	[COMMAND_RECORD_BUFFER_ATTR_CHANGED] = { "RECORD_BUFFER_ATTR_CHANGED", },

	/* Supported since protocol v16 (0.9.16) */
	[COMMAND_SET_SINK_PORT] = { "SET_SINK_PORT", do_error_access, },
	[COMMAND_SET_SOURCE_PORT] = { "SET_SOURCE_PORT", do_error_access, },

	/* Supported since protocol v22 (1.0) */
	[COMMAND_SET_SOURCE_OUTPUT_VOLUME] = { "SET_SOURCE_OUTPUT_VOLUME",  do_set_stream_volume, },
	[COMMAND_SET_SOURCE_OUTPUT_MUTE] = { "SET_SOURCE_OUTPUT_MUTE",  do_set_stream_mute, },

	/* Supported since protocol v27 (3.0) */
	[COMMAND_SET_PORT_LATENCY_OFFSET] = { "SET_PORT_LATENCY_OFFSET", do_error_access, },

	/* Supported since protocol v30 (6.0) */
	/* BOTH DIRECTIONS */
	[COMMAND_ENABLE_SRBCHANNEL] = { "ENABLE_SRBCHANNEL", do_error_access, },
	[COMMAND_DISABLE_SRBCHANNEL] = { "DISABLE_SRBCHANNEL", do_error_access, },

	/* Supported since protocol v31 (9.0)
	 * BOTH DIRECTIONS */
	[COMMAND_REGISTER_MEMFD_SHMID] = { "REGISTER_MEMFD_SHMID", do_error_access, },
};

static void client_free(struct client *client)
{
	struct impl *impl = client->impl;
	struct message *msg;

	pw_log_info(NAME" %p: client %p free", impl, client);
	spa_list_remove(&client->link);
	pw_map_clear(&client->streams);
	spa_list_consume(msg, &client->free_messages, link)
		message_free(client, msg, true);
	spa_list_consume(msg, &client->out_messages, link)
		message_free(client, msg, true);
	if (client->core)
		pw_core_disconnect(client->core);
	if (client->props)
		pw_properties_free(client->props);
	if (client->source)
		pw_loop_destroy_source(impl->loop, client->source);
	free(client);
}

static int handle_packet(struct client *client, struct message *msg)
{
	struct impl *impl = client->impl;
	int res = 0;
	uint32_t command, tag;

	if (message_get(msg,
			TAG_U32, &command,
			TAG_U32, &tag,
			TAG_INVALID) < 0) {
		res = -EPROTO;
		goto finish;
	}

	pw_log_debug(NAME" %p: Received packet command %u tag %u",
			impl, command, tag);

	if (command >= COMMAND_MAX) {
		pw_log_error(NAME" %p: invalid command %d",
				impl, command);
		res = -EINVAL;
		goto finish;

	}
	if (commands[command].run == NULL) {
		pw_log_error(NAME" %p: command %d (%s) not implemented",
				impl, command, commands[command].name);
		res = -ENOTSUP;
		goto finish;
	}

	res = commands[command].run(client, command, tag, msg);
	if (res < 0) {
		pw_log_error(NAME" %p: command %d (%s) error: %s",
				impl, command, commands[command].name, spa_strerror(res));
	}

finish:
	return res;
}

static int handle_memblock(struct client *client, struct message *msg)
{
	struct impl *impl = client->impl;
	struct stream *stream;
	uint32_t channel, flags;
	int64_t offset;

	channel = ntohl(client->desc.channel);
	offset = (int64_t) (
             (((uint64_t) ntohl(client->desc.offset_hi)) << 32) |
             (((uint64_t) ntohl(client->desc.offset_lo))));
	flags = ntohl(client->desc.flags) & FLAG_SEEKMASK,

	pw_log_debug(NAME" %p: Received memblock channel:%d offset:%"PRIi64
			" flags:%08x size:%u", impl, channel, offset,
			flags, msg->length);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	pw_log_debug("new block %p %p", msg, msg->data);
	spa_list_append(&stream->messages, &msg->link);
	stream->write_index += msg->length;

	return 0;
}

static int do_read(struct client *client)
{
	struct impl *impl = client->impl;
	void *data;
	size_t size;
	ssize_t r;
	int res = 0;

	if (client->in_index < sizeof(client->desc)) {
		data = SPA_MEMBER(&client->desc, client->in_index, void);
		size = sizeof(client->desc) - client->in_index;
	} else {
		uint32_t idx = client->in_index - sizeof(client->desc);

		if (client->message == NULL) {
			res = -EIO;
			goto error;
		}
		data = SPA_MEMBER(client->message->data, idx, void);
		size = client->message->length - idx;
	}
	while (true) {
		if ((r = recv(client->source->fd, data, size, 0)) < 0) {
			if (errno == EINTR)
		                continue;
			res = -errno;
			goto error;
		}
		client->in_index += r;
		break;
	}

	if (client->in_index == sizeof(client->desc)) {
		uint32_t flags, length, channel;

		flags = ntohl(client->desc.flags);
		if ((flags & FLAG_SHMMASK) != 0) {
			res = -ENOTSUP;
			goto error;
		}

		length = ntohl(client->desc.length);
		if (length > FRAME_SIZE_MAX_ALLOW || length <= 0) {
			pw_log_warn(NAME" %p: Received invalid frame size: %u",
					impl, length);
			res = -EPROTO;
			goto error;
		}
		channel = ntohl(client->desc.channel);
		if (channel == (uint32_t) -1) {
			if (flags != 0) {
				pw_log_warn(NAME" %p: Received packet frame with invalid "
						"flags value.", impl);
				res = -EPROTO;
				goto error;
			}
		}
		if (client->message)
			message_free(client, client->message, false);
		client->message = message_alloc(client, length, channel);
	} else if (client->message &&
	    client->in_index >= client->message->length + sizeof(client->desc)) {
		struct message *msg = client->message;

		client->message = NULL;
		client->in_index = 0;

		if (msg->channel == (uint32_t)-1)
			res = handle_packet(client, msg);
		else
			res = handle_memblock(client, msg);
	}
error:
	return res;
}

static void
on_client_data(void *data, int fd, uint32_t mask)
{
	struct client *client = data;
	struct impl *impl = client->impl;
	int res;

	if (mask & SPA_IO_HUP) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_ERR) {
		res = -EIO;
		goto error;
	}
	if (mask & SPA_IO_OUT) {
		pw_log_trace(NAME" %p: can write", impl);
		res = flush_messages(client);
		if (res >= 0) {
			int mask = client->source->mask;
			SPA_FLAG_CLEAR(mask, SPA_IO_OUT);
			pw_loop_update_io(impl->loop, client->source, mask);
		} else if (res != EAGAIN)
			goto error;
	}
	if (mask & SPA_IO_IN) {
		pw_log_trace(NAME" %p: can read", impl);
		if ((res = do_read(client)) < 0)
			goto error;
	}
	return;

error:
        if (res == -EPIPE)
                pw_log_info(NAME" %p: client %p disconnected", impl, client);
        else
                pw_log_error(NAME" %p: client %p error %d (%s)", impl,
                                client, res, spa_strerror(res));
	client_free(client);
}

static void
on_connect(void *data, int fd, uint32_t mask)
{
        struct impl *impl = data;
        struct sockaddr_un name;
        socklen_t length;
        int client_fd;
	struct client *client;

	client = calloc(1, sizeof(struct client));
	if (client == NULL)
		goto error;

	client->impl = impl;
	spa_list_append(&impl->clients, &client->link);
	pw_map_init(&client->streams, 16, 16);
	spa_list_init(&client->free_messages);
	spa_list_init(&client->out_messages);

	client->props = pw_properties_new(
			PW_KEY_CLIENT_API, "pipewire-pulse",
			NULL);
	if (client->props == NULL)
		goto error;

        length = sizeof(name);
        client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
        if (client_fd < 0)
                goto error;

	pw_log_info(NAME": client %p fd:%d", client, client_fd);

	client->source = pw_loop_add_io(impl->loop,
					client_fd,
					SPA_IO_ERR | SPA_IO_HUP | SPA_IO_IN,
					true, on_client_data, client);
	if (client->source == NULL)
		goto error;

	client->core = pw_context_connect(impl->context,
			pw_properties_copy(client->props), 0);
	if (client->core == NULL)
		goto error;

	return;
error:
	pw_log_error(NAME" %p: failed to create client: %m", impl);
	if (client)
		client_free(client);
	return;
}

static const char *
get_runtime_dir(void)
{
	const char *runtime_dir;

	runtime_dir = getenv("PULSE_RUNTIME_PATH");
	if (runtime_dir == NULL)
		runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir == NULL)
		runtime_dir = getenv("HOME");
	if (runtime_dir == NULL) {
		struct passwd pwd, *result = NULL;
		char buffer[4096];
		if (getpwuid_r(getuid(), &pwd, buffer, sizeof(buffer), &result) == 0)
			runtime_dir = result ? result->pw_dir : NULL;
	}
	return runtime_dir;
}

static int create_server(struct impl *impl, const char *name)
{
	const char *runtime_dir;
	socklen_t size;
	struct sockaddr_un addr;
	int name_size, fd, res;

	runtime_dir = get_runtime_dir();

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof(addr.sun_path),
                             "%s/pulse/%s", runtime_dir, name) + 1;
	if (name_size > (int) sizeof(addr.sun_path)) {
		pw_log_error(NAME" %p: %s/%s too long",
					impl, runtime_dir, name);
		res = -ENAMETOOLONG;
		goto error;
	}

	struct stat socket_stat;

	if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		res = -errno;
		goto error;
	}
	if (stat(addr.sun_path, &socket_stat) < 0) {
		if (errno != ENOENT) {
			res = -errno;
			pw_log_error("server %p: stat %s failed with error: %m",
					impl, addr.sun_path);
			goto error_close;
		}
	} else if (socket_stat.st_mode & S_IWUSR || socket_stat.st_mode & S_IWGRP) {
		unlink(addr.sun_path);
	}

	size = offsetof(struct sockaddr_un, sun_path) + strlen(addr.sun_path);
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		res = -errno;
		pw_log_error(NAME" %p: bind() failed with error: %m", impl);
		goto error_close;
	}
	if (listen(fd, 128) < 0) {
		res = -errno;
		pw_log_error(NAME" %p: listen() failed with error: %m", impl);
		goto error_close;
	}
	impl->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, true, on_connect, impl);
	if (impl->source == NULL) {
		res = -errno;
		pw_log_error(NAME" %p: can't create source: %m", impl);
		goto error_close;
	}
	pw_log_info(NAME" listening on %s", addr.sun_path);
	return 0;

error_close:
	close(fd);
error:
	return res;

}


static void session_destroy(void *data)
{
	struct impl *impl = data;
	struct client *c;

	spa_list_consume(c, &impl->clients, link)
		client_free(c);
	spa_hook_remove(&impl->listener);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.destroy = session_destroy,
};

int sm_pulse_bridge_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->loop = session->loop;
	impl->context = session->context;
	spa_list_init(&impl->clients);

	impl->default_sink = (struct device) {
		.index = 1,
		.name = "input.pipewire",
		.direction = PW_DIRECTION_INPUT,
		.props = pw_properties_new(
				"device.description", "PipeWire Sink",
				NULL),
		.ss = (struct sample_spec) {
			.format = SAMPLE_FLOAT32LE,
			.rate = 44100,
			.channels = 2, },
		.volume = (struct cvolume) {
			.channels = 2,
			.values[0] = 1.0f,
			.values[1] = 1.0f, },
		.map = (struct channel_map) {
			.channels = 2,
			.map[0] = 1,
			.map[1] = 2, },
		.muted = false,
	};
	impl->default_source = (struct device) {
		.index = 2,
		.name = "output.pipewire",
		.direction = PW_DIRECTION_OUTPUT,
		.props = pw_properties_new(
				"device.description", "PipeWire Source",
				NULL),
		.ss = (struct sample_spec) {
			.format = SAMPLE_FLOAT32LE,
			.rate = 44100,
			.channels = 2, },
		.volume = (struct cvolume) {
			.channels = 2,
			.values[0] = 1.0f,
			.values[1] = 1.0f, },
		.map = (struct channel_map) {
			.channels = 2,
			.map[0] = 1,
			.map[1] = 2, },
		.muted = false,
	};

	sm_media_session_add_listener(impl->session,
			&impl->listener,
			&session_events, impl);

	if ((res = create_server(impl, "native")) < 0)
		return res;

	return 0;
}
