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

struct impl {
	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_loop *loop;
	struct pw_context *context;
        struct spa_source *source;

	struct spa_list clients;
};

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

struct data {
	uint8_t *data;
	uint32_t length;
	uint32_t offset;
};

struct client {
	struct spa_list link;
	struct impl *impl;

        struct spa_source *source;

	uint32_t version;

	struct pw_properties *props;

	struct pw_core *core;

	uint32_t index;
	struct descriptor desc;

#define TYPE_PACKET	0
#define TYPE_MEMBLOCK	1
	uint32_t type;
	struct data data;

	struct pw_map streams;
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

struct buffer_attr {
	uint32_t maxlength;
	uint32_t tlength;
	uint32_t prebuf;
	uint32_t minreq;
	uint32_t fragsize;
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

struct block {
	struct spa_list link;
	uint8_t *data;
	uint32_t length;
	uint32_t offset;
};

struct stream {
	uint32_t create_tag;
	uint32_t channel;	/* index in map */

	struct impl *impl;
	struct client *client;
	enum pw_direction direction;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_list blocks;
	int64_t read_index;
	int64_t write_index;
	uint64_t playing_for;

	struct sample_spec ss;
	struct channel_map map;
	struct buffer_attr attr;
	uint32_t frame_size;

	uint32_t drain_tag;
	unsigned int corked:1;
	unsigned int adjust_latency:1;
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
	int (*run) (struct client *client, uint32_t command, uint32_t tag, struct data *d);
};
static const struct command commands[COMMAND_MAX];

static int data_get(struct data *d, ...);

static int read_u8(struct data *d, uint8_t *val)
{
	if (d->offset + 1 > d->length)
		return -ENOSPC;
	*val = d->data[d->offset];
	d->offset++;
	return 0;
}

static int read_u32(struct data *d, uint32_t *val)
{
	if (d->offset + 4 > d->length)
		return -ENOSPC;
	memcpy(val, &d->data[d->offset], 4);
	*val = ntohl(*val);
	d->offset += 4;
	return 0;
}
static int read_u64(struct data *d, uint64_t *val)
{
	uint32_t tmp;
	int res;
	if ((res = read_u32(d, &tmp)) < 0)
		return res;
	*val = ((uint64_t)tmp) << 32;
	if ((res = read_u32(d, &tmp)) < 0)
		return res;
	*val |= tmp;
	return 0;
}

static int read_sample_spec(struct data *d, struct sample_spec *ss)
{
	int res;
	uint8_t tmp;
	if ((res = read_u8(d, &tmp)) < 0)
		return res;
	ss->format = tmp;
	if ((res = read_u8(d, &ss->channels)) < 0)
		return res;
	return read_u32(d, &ss->rate);
}

static int read_props(struct data *d, struct pw_properties *props)
{
	int res;

	while (true) {
		char *key;
		void *data;
		uint32_t length;

		if ((res = data_get(d,
				TAG_STRING, &key,
				TAG_INVALID)) < 0)
			return res;

		if (key == NULL)
			break;

		if ((res = data_get(d,
				TAG_U32, &length,
				TAG_INVALID)) < 0)
			return res;
		if (length > MAX_TAG_SIZE)
			return -EINVAL;

		if ((res = data_get(d,
				TAG_ARBITRARY, &data, length,
				TAG_INVALID)) < 0)
			return res;

		pw_log_debug("%s %s", key, (char*)data);
		pw_properties_set(props, key, data);
	}
	return 0;
}

static int read_arbitrary(struct data *d, const void **val, size_t length)
{
	uint32_t len;
	int res;
	if ((res = read_u32(d, &len)) < 0)
		return res;
	if (len != length)
		return -EINVAL;
	if (d->offset + length > d->length)
		return -ENOSPC;
	*val = d->data + d->offset;
	d->offset += length;
	return 0;
}

static int read_string(struct data *d, char **str)
{
	uint32_t n, maxlen = d->length - d->offset;
	n = strnlen(d->data + d->offset, maxlen);
	if (n == maxlen)
		return -EINVAL;
	*str = d->data + d->offset;
	d->offset += n + 1;
	return 0;
}

static int read_timeval(struct data *d, struct timeval *tv)
{
	int res;
	uint32_t tmp;

	if ((res = read_u32(d, &tmp)) < 0)
		return res;
	tv->tv_sec = tmp;
	if ((res = read_u32(d, &tmp)) < 0)
		return res;
	tv->tv_usec = tmp;
	return 0;
}

static int read_channel_map(struct data *d, struct channel_map *map)
{
	int res;
	uint8_t i, tmp;

	if ((res = read_u8(d, &map->channels)) < 0)
		return res;
	if (map->channels > CHANNELS_MAX)
		return -EINVAL;
	for (i = 0; i < map->channels; i ++) {
		if ((res = read_u8(d, &tmp)) < 0)
			return res;
		map->map[i] = tmp;
	}
	return 0;
}
static int read_volume(struct data *d, float *vol)
{
	int res;
	uint32_t v;
	if ((res = read_u32(d, &v)) < 0)
		return res;
	*vol = ((float)v) / 0x10000U;
	return 0;
}

static int read_cvolume(struct data *d, struct cvolume *vol)
{
	int res;
	uint8_t i;

	if ((res = read_u8(d, &vol->channels)) < 0)
		return res;
	if (vol->channels > CHANNELS_MAX)
		return -EINVAL;
	for (i = 0; i < vol->channels; i ++) {
		if ((res = read_volume(d, &vol->values[i])) < 0)
			return res;
	}
	return 0;
}

static int read_format_info(struct data *d, struct format_info *info)
{
	int res;
	uint8_t tag, encoding;

	if ((res = read_u8(d, &tag)) < 0)
		return res;
	if (tag != TAG_U8)
		return -EPROTO;
	if ((res = read_u8(d, &encoding)) < 0)
		return res;
	info->encoding = encoding;

	if ((res = read_u8(d, &tag)) < 0)
		return res;
	if (tag != TAG_PROPLIST)
		return -EPROTO;

	info->props = pw_properties_new(NULL, NULL);
	if (info->props == NULL)
		return -errno;
	return read_props(d, info->props);
}

static int data_get(struct data *d, ...)
{
	va_list va;
	int res;

	va_start(va, d);

	while (true) {
		int tag = va_arg(va, int);
		uint8_t dtag;
		if (tag == TAG_INVALID)
			break;

		if ((res = read_u8(d, &dtag)) < 0)
			return res;

		switch (dtag) {
		case TAG_STRING:
			if (tag != TAG_STRING)
				return -EINVAL;
			if ((res = read_string(d, va_arg(va, char**))) < 0)
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
			if ((res = read_u8(d, va_arg(va, uint8_t*))) < 0)
				return res;
			break;
		case TAG_U32:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u32(d, va_arg(va, uint32_t*))) < 0)
				return res;
			break;
		case TAG_S64:
		case TAG_U64:
		case TAG_USEC:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u64(d, va_arg(va, uint64_t*))) < 0)
				return res;
			break;
		case TAG_SAMPLE_SPEC:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_sample_spec(d, va_arg(va, struct sample_spec*))) < 0)
				return res;
			break;
		case TAG_ARBITRARY:
		{
			const void **val = va_arg(va, const void**);
			size_t len = va_arg(va, size_t);
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_arbitrary(d, val, len)) < 0)
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
			if ((res = read_timeval(d, va_arg(va, struct timeval*))) < 0)
				return res;
			break;
		case TAG_CHANNEL_MAP:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_channel_map(d, va_arg(va, struct channel_map*))) < 0)
				return res;
			break;
		case TAG_CVOLUME:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_cvolume(d, va_arg(va, struct cvolume*))) < 0)
				return res;
			break;
		case TAG_PROPLIST:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_props(d, va_arg(va, struct pw_properties*))) < 0)
				return res;
			break;
		case TAG_VOLUME:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_volume(d, va_arg(va, float*))) < 0)
				return res;
			break;
		case TAG_FORMAT_INFO:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_format_info(d, va_arg(va, struct format_info*))) < 0)
				return res;
			break;
		}
	}
	va_end(va);

	return 0;
}

static void write_8(struct data *d, uint8_t val)
{
	if (d->offset < d->length)
		d->data[d->offset] = val;
	d->offset++;
}

static void write_32(struct data *d, uint32_t val)
{
	val = htonl(val);
	if (d->offset + 4 <= d->length)
		memcpy(d->data + d->offset, &val, 4);
	d->offset += 4;
}

static void write_string(struct data *d, const char *s)
{
	write_8(d, s ? TAG_STRING : TAG_STRING_NULL);
	if (s != NULL) {
		int len = strlen(s) + 1;
		if (d->offset + len <= d->length)
			strcpy(&d->data[d->offset], s);
		d->offset += len;
	}
}
static void write_u8(struct data *d, uint8_t val)
{
	write_8(d, TAG_U8);
	write_8(d, val);
}

static void write_u32(struct data *d, uint32_t val)
{
	write_8(d, TAG_U32);
	write_32(d, val);
}

static void write_64(struct data *d, uint8_t tag, uint64_t val)
{
	write_8(d, tag);
	write_32(d, val >> 32);
	write_32(d, val);
}

static void write_sample_spec(struct data *d, struct sample_spec *ss)
{
	write_8(d, TAG_SAMPLE_SPEC);
	write_8(d, ss->format);
	write_8(d, ss->channels);
	write_32(d, ss->rate);
}

static void write_arbitrary(struct data *d, const void *p, size_t length)
{
	write_8(d, TAG_ARBITRARY);
	write_32(d, length);
	if (length > 0 && d->offset + length <= d->length)
		memcpy(d->data + d->offset, p, length);
	d->offset += length;
}

static void write_boolean(struct data *d, bool val)
{
	write_8(d, val ? TAG_BOOLEAN_TRUE : TAG_BOOLEAN_FALSE);
}

static void write_timeval(struct data *d, struct timeval *tv)
{
	write_8(d, TAG_TIMEVAL);
	write_32(d, tv->tv_sec);
	write_32(d, tv->tv_usec);
}

static void write_channel_map(struct data *d, struct channel_map *map)
{
	uint8_t i;
	write_8(d, TAG_CHANNEL_MAP);
	write_8(d, map->channels);
	for (i = 0; i < map->channels; i ++)
		write_8(d, map->map[i]);
}

static void write_volume(struct data *d, float vol)
{
	write_8(d, TAG_VOLUME);
	write_32(d, vol * 0x10000U);
}

static void write_cvolume(struct data *d, struct cvolume *cvol)
{
	uint8_t i;
	write_8(d, TAG_CVOLUME);
	write_8(d, cvol->channels);
	for (i = 0; i < cvol->channels; i ++)
		write_32(d, cvol->values[i] * 0x10000U);
}

static void write_props(struct data *d, struct pw_properties *props)
{
	const struct spa_dict_item *it;
	write_8(d, TAG_PROPLIST);
	if (props != NULL) {
		spa_dict_for_each(it, &props->dict) {
			int l = strlen(it->value);
			write_string(d, it->key);
			write_u32(d, l+1);
			write_arbitrary(d, it->value, l+1);
		}
	}
	write_string(d, NULL);
}

static void write_format_info(struct data *d, struct format_info *info)
{
	write_8(d, TAG_FORMAT_INFO);
	write_u8(d, (uint8_t) info->encoding);
	write_props(d, info->props);
}

static int data_put(struct data *d, ...)
{
	va_list va;

	va_start(va, d);

	while (true) {
		int tag = va_arg(va, int);
		if (tag == TAG_INVALID)
			break;

		switch (tag) {
		case TAG_STRING:
			write_string(d, va_arg(va, const char *));
			break;
		case TAG_U8:
			write_u8(d, (uint8_t)va_arg(va, int));
			break;
		case TAG_U32:
			write_u32(d, (uint32_t)va_arg(va, uint32_t));
			break;
		case TAG_S64:
		case TAG_U64:
		case TAG_USEC:
			write_64(d, tag, va_arg(va, uint64_t));
			break;
		case TAG_SAMPLE_SPEC:
			write_sample_spec(d, va_arg(va, struct sample_spec*));
			break;
		case TAG_ARBITRARY:
		{
			const void *p = va_arg(va, const void*);
			size_t length = va_arg(va, size_t);
			write_arbitrary(d, p, length);
			break;
		}
		case TAG_BOOLEAN:
			write_boolean(d, va_arg(va, int));
			break;
		case TAG_TIMEVAL:
			write_timeval(d, va_arg(va, struct timeval*));
			break;
		case TAG_CHANNEL_MAP:
			write_channel_map(d, va_arg(va, struct channel_map*));
			break;
		case TAG_CVOLUME:
			write_cvolume(d, va_arg(va, struct cvolume*));
			break;
		case TAG_PROPLIST:
			write_props(d, va_arg(va, struct pw_properties*));
			break;
		case TAG_VOLUME:
			write_volume(d, va_arg(va, double));
			break;
		case TAG_FORMAT_INFO:
			write_format_info(d, va_arg(va, struct format_info*));
			break;
		}
	}
	va_end(va);

	return 0;
}


static int send_data(struct client *client, struct data *d)
{
	struct descriptor desc;

	desc.length = htonl(d->offset);
	desc.channel = htonl(-1);
	desc.offset_hi = 0;
	desc.offset_lo = 0;
	desc.flags = 0;
	send(client->source->fd, &desc, sizeof(desc), MSG_NOSIGNAL);
	send(client->source->fd, d->data, d->offset, MSG_NOSIGNAL);
	return 0;
}

static int do_command_auth(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t version;
	const void *cookie;
	int res;

	if ((res = data_get(d,
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

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	pw_log_info(NAME" %p: AUTH version:%d", impl, version);

	data_put(&reply,
			TAG_U32, COMMAND_REPLY,
			TAG_U32, tag,
			TAG_U32, PROTOCOL_VERSION,
			TAG_INVALID);

	return send_data(client, &reply);
}

static int do_set_client_name(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	const char *name = NULL;
	int res, changed = 0;

	if (client->version < 13) {
		if ((res = data_get(d,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			return res;
		if (name)
			changed += pw_properties_set(client->props, "application.name", name);
	} else {
		if ((res = data_get(d,
				TAG_PROPLIST, client->props,
				TAG_INVALID)) < 0)
			return res;
		changed++;
	}
	if (changed)
		pw_core_update_properties(client->core, &client->props->dict);

	pw_log_info(NAME" %p: SET_CLIENT_NAME %s", impl,
			pw_properties_get(client->props, "application.name"));

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
			TAG_U32, COMMAND_REPLY,
			TAG_U32, tag,
			TAG_INVALID);

	if (client->version >= 13) {
		data_put(&reply,
			TAG_U32, 0,	/* client index */
			TAG_INVALID);
	}
	return send_data(client, &reply);
}

static int do_subscribe(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t mask;
	int res;

	if ((res = data_get(d,
			TAG_U32, &mask,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: SUBSCRIBE mask:%08x", impl, mask);

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
			TAG_U32, COMMAND_REPLY,
			TAG_U32, tag,
			TAG_INVALID);

	return send_data(client, &reply);
}

static void block_free(struct block *block)
{
	spa_list_remove(&block->link);
	free(block->data);
	free(block);
}

static void stream_flush(struct stream *stream)
{
	struct block *block;
	spa_list_consume(block, &stream->blocks, link)
		block_free(block);
	if (stream->direction == PW_DIRECTION_INPUT)
		stream->read_index = stream->write_index;
	else
		stream->write_index = stream->read_index;
	stream->playing_for = 0;
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
	uint8_t buffer[1024];
	struct data msg;
	uint32_t size;

	size = writable_size(stream, 0);
	if (size == 0)
		return 0;

	pw_log_trace(NAME" %p: REQUEST channel:%d %u", stream, stream->channel, size);

	spa_zero(msg);
	msg.data = buffer;
	msg.length = sizeof(buffer);

	data_put(&msg,
		TAG_U32, COMMAND_REQUEST,
		TAG_U32, -1,
		TAG_U32, stream->channel,
		TAG_U32, size,
		TAG_INVALID);

	return send_data(client, &msg);
}

static int reply_simple_ack(struct client *client, uint32_t tag)
{
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	pw_log_debug(NAME" %p: REPLY tag:%u", client, tag);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);

	return send_data(client, &reply);
}

static int reply_error(struct client *client, uint32_t tag, uint32_t error)
{
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	pw_log_debug(NAME" %p: ERROR tag:%u error:%u", client, tag, error);

	data_put(&reply,
		TAG_U32, COMMAND_ERROR,
		TAG_U32, tag,
		TAG_U32, error,
		TAG_INVALID);

	return send_data(client, &reply);
}

static int reply_create_playback_stream(struct stream *stream)
{
	struct client *client = stream->client;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t size;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	size = writable_size(stream, 0);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, stream->create_tag,
		TAG_U32, stream->channel,	/* stream index/channel */
		TAG_U32, 0,			/* sink_input/stream index */
		TAG_U32, size,			/* missing/requested bytes */
		TAG_INVALID);

	if (client->version >= 9) {
		data_put(&reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_INVALID);
	}
	if (client->version >= 12) {
		data_put(&reply,
			TAG_SAMPLE_SPEC, &stream->ss,
			TAG_CHANNEL_MAP, &stream->map,
			TAG_U32, 0,			/* sink index */
			TAG_STRING, "sink",		/* sink name */
			TAG_BOOLEAN, false,		/* sink suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		data_put(&reply,
			TAG_USEC, 0ULL,			/* sink configured latency */
			TAG_INVALID);
	}
	if (client->version >= 21) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		data_put(&reply,
			TAG_FORMAT_INFO, &info,		/* sink_input format */
			TAG_INVALID);
	}

	stream->create_tag = SPA_ID_INVALID;

	return send_data(client, &reply);
}

static int reply_create_record_stream(struct stream *stream)
{
	struct client *client = stream->client;
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, stream->create_tag,
		TAG_U32, stream->channel,	/* stream index/channel */
		TAG_U32, 0,			/* source_output/stream index */
		TAG_INVALID);

	if (client->version >= 9) {
		data_put(&reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.fragsize,
			TAG_INVALID);
	}
	if (client->version >= 12) {
		data_put(&reply,
			TAG_SAMPLE_SPEC, &stream->ss,
			TAG_CHANNEL_MAP, &stream->map,
			TAG_U32, 0,			/* source index */
			TAG_STRING, "sink",		/* source name */
			TAG_BOOLEAN, false,		/* source suspended state */
			TAG_INVALID);
	}
	if (client->version >= 13) {
		data_put(&reply,
			TAG_USEC, 0ULL,			/* source configured latency */
			TAG_INVALID);
	}
	if (client->version >= 22) {
		struct format_info info;
		spa_zero(info);
		info.encoding = ENCODING_PCM;
		data_put(&reply,
			TAG_FORMAT_INFO, &info,		/* source_output format */
			TAG_INVALID);
	}

	stream->create_tag = SPA_ID_INVALID;

	return send_data(client, &reply);
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

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct stream *stream = data;

	if (id != SPA_PARAM_Format || param == NULL)
		return;

	stream->frame_size = sample_spec_frame_size(&stream->ss);

	if (stream->create_tag != SPA_ID_INVALID) {
		if (stream->corked)
			pw_stream_set_active(stream->stream, false);
		if (stream->direction == PW_DIRECTION_OUTPUT)
			reply_create_playback_stream(stream);
		else
			reply_create_record_stream(stream);
	}
}

static void stream_process(void *data)
{
	struct stream *stream = data;
	struct block *block;
	struct pw_buffer *buffer;
	struct spa_buffer *buf;
	uint32_t size, maxsize;
	void *p;

	pw_log_trace(NAME" %p: process", stream);

	while (!spa_list_is_empty(&stream->blocks)) {
		buffer = pw_stream_dequeue_buffer(stream->stream);
		if (buffer == NULL)
			break;

	        buf = buffer->buffer;
	        if ((p = buf->datas[0].data) == NULL)
			break;

		block = spa_list_first(&stream->blocks, struct block, link);
		maxsize = buf->datas[0].maxsize;
		size = SPA_MIN(block->length - block->offset, maxsize);
		memcpy(p, block->data + block->offset, size);

		pw_log_trace(NAME" %p: process block %p %d-%d/%d",
				stream, block, block->offset, size, block->length);

		stream->read_index += size;
		stream->playing_for += size;
		block->offset += size;
		if (block->offset >= block->length)
			block_free(block);

	        buf->datas[0].chunk->offset = 0;
	        buf->datas[0].chunk->stride = stream->frame_size;
	        buf->datas[0].chunk->size = size;

		pw_stream_queue_buffer(stream->stream, buffer);
	}
	send_command_request(stream);
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

static size_t usec_to_bytes_round_up(uint64_t usec, const struct sample_spec *ss)
{
	uint64_t u;
	u = (uint64_t) usec * (uint64_t) ss->rate;
	u = (u + 1000000UL - 1) / 1000000UL;
	u *= sample_spec_frame_size(ss);
	return (size_t) u;
}

static void fix_playback_buffer_attr(struct stream *s, struct buffer_attr *attr)
{
	size_t frame_size, max_prebuf;

	frame_size = sample_spec_frame_size(&s->ss);

	if (attr->maxlength == (uint32_t) -1 || attr->maxlength > MAXLENGTH)
		attr->maxlength = MAXLENGTH;
	if (attr->maxlength <= 0)
		attr->maxlength = (uint32_t) frame_size;

	if (attr->tlength == (uint32_t) -1)
		attr->tlength = (uint32_t) usec_to_bytes_round_up(DEFAULT_TLENGTH_MSEC*1000, &s->ss);

	if (attr->tlength <= 0)
		attr->tlength = (uint32_t) frame_size;
	if (attr->tlength > attr->maxlength)
		attr->tlength = attr->maxlength;

	if (attr->minreq == (uint32_t) -1) {
		uint32_t process = (uint32_t) usec_to_bytes_round_up(DEFAULT_PROCESS_MSEC*1000, &s->ss);
		/* With low-latency, tlength/4 gives a decent default in all of traditional,
		 * adjust latency and early request modes. */
		uint32_t m = attr->tlength / 4;
		if (frame_size)
			m -= m % frame_size;
		attr->minreq = SPA_MIN(process, m);
	}
	if (attr->minreq <= 0)
		attr->minreq = (uint32_t) frame_size;

	if (attr->tlength < attr->minreq+frame_size)
		attr->tlength = attr->minreq+(uint32_t) frame_size;


	if (attr->minreq <= 0) {
		attr->minreq = (uint32_t) frame_size;
		attr->tlength += (uint32_t) frame_size*2;
	}

	if (attr->tlength <= attr->minreq)
		attr->tlength = attr->minreq*2 + (uint32_t) frame_size;

	max_prebuf = attr->tlength + (uint32_t)frame_size - attr->minreq;

	if (attr->prebuf == (uint32_t) -1 ||
		attr->prebuf > max_prebuf)
	attr->prebuf = max_prebuf;
}

static int do_create_playback_stream(struct client *client, uint32_t command, uint32_t tag, struct data *d)
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
		if ((res = data_get(d,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			goto error;
		if (name == NULL) {
			res = -EPROTO;
			goto error;
		}
	}
	if ((res = data_get(d,
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

	pw_log_info(NAME" %p: CREATE_PLAYBACK_STREAM corked:%u", impl, corked);

	if (client->version >= 12) {
		if ((res = data_get(d,
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
		if ((res = data_get(d,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 14) {
		if ((res = data_get(d,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 15) {
		if ((res = data_get(d,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 17) {
		if ((res = data_get(d,
				TAG_BOOLEAN, &relative_volume,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 18) {
		if ((res = data_get(d,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 21) {
		if ((res = data_get(d,
				TAG_U8, &n_formats,
				TAG_INVALID)) < 0)
			goto error;

		if (n_formats) {
			uint8_t i;
			formats = calloc(n_formats, sizeof(struct format_info));
			for (i = 0; i < n_formats; i++) {
				if ((res = data_get(d,
						TAG_FORMAT_INFO, &formats[i],
						TAG_INVALID)) < 0)
					goto error;
			}
		}
	}
	if (d->offset != d->length) {
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
	spa_list_init(&stream->blocks);

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL) {
		res = -errno;
		goto error;
	}
	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

	stream->direction = PW_DIRECTION_OUTPUT;
	stream->create_tag = tag;
	stream->ss = ss;
	stream->map = map;

	fix_playback_buffer_attr(stream, &attr);
	stream->attr = attr;

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

static int do_create_record_stream(struct client *client, uint32_t command, uint32_t tag, struct data *d)
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

	pw_log_info(NAME" %p: CREATE_PLAYBACK_STREAM", impl);

	props = pw_properties_new(NULL, NULL);

	if (client->version < 13) {
		if ((res = data_get(d,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			goto error;
		if (name == NULL) {
			res = -EPROTO;
			goto error;
		}
	}
	if ((res = data_get(d,
			TAG_SAMPLE_SPEC, &ss,
			TAG_CHANNEL_MAP, &map,
			TAG_U32, &source_index,
			TAG_STRING, &source_name,
			TAG_U32, &attr.maxlength,
			TAG_BOOLEAN, &corked,
			TAG_U32, &attr.fragsize,
			TAG_INVALID)) < 0)
		goto error;

	if (client->version >= 12) {
		if ((res = data_get(d,
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
		if ((res = data_get(d,
				TAG_BOOLEAN, &peak_detect,
				TAG_BOOLEAN, &adjust_latency,
				TAG_PROPLIST, props,
				TAG_U32, &direct_on_input_idx,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 14) {
		if ((res = data_get(d,
				TAG_BOOLEAN, &early_requests,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 15) {
		if ((res = data_get(d,
				TAG_BOOLEAN, &dont_inhibit_auto_suspend,
				TAG_BOOLEAN, &fail_on_suspend,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (client->version >= 22) {
		if ((res = data_get(d,
				TAG_U8, &n_formats,
				TAG_INVALID)) < 0)
			goto error;

		if (n_formats) {
			uint8_t i;
			formats = calloc(n_formats, sizeof(struct format_info));
			for (i = 0; i < n_formats; i++) {
				if ((res = data_get(d,
						TAG_FORMAT_INFO, &formats[i],
						TAG_INVALID)) < 0)
					goto error;
			}
		}
		if ((res = data_get(d,
				TAG_CVOLUME, &volume,
				TAG_BOOLEAN, &muted,
				TAG_BOOLEAN, &volume_set,
				TAG_BOOLEAN, &muted_set,
				TAG_BOOLEAN, &relative_volume,
				TAG_BOOLEAN, &passthrough,
				TAG_INVALID)) < 0)
			goto error;
	}
	if (d->offset != d->length) {
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
	stream->channel = pw_map_insert_new(&client->streams, stream);
	spa_list_init(&stream->blocks);

	stream->stream = pw_stream_new(client->core, name, props);
	props = NULL;
	if (stream->stream == NULL) {
		res = -errno;
		goto error;
	}
	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			&stream_events, stream);

	stream->create_tag = tag;
	stream->ss = ss;
	stream->map = map;
	stream->attr = attr;

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

static int do_delete_stream(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
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

static int do_get_playback_latency(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t channel;
	struct timeval tv, now;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
			TAG_U32, &channel,
			TAG_TIMEVAL, &tv,
			TAG_INVALID)) < 0)
		return res;

	pw_log_debug(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	pw_log_debug("read:%"PRIi64" write:%"PRIi64" queued:%"PRIi64,
			stream->read_index, stream->write_index,
			stream->write_index - stream->read_index);

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	gettimeofday(&now, NULL);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_USEC, 0,		/* sink latency + queued samples */
		TAG_USEC, 0,		/* always 0 */
		TAG_BOOLEAN, true,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &now,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	if (client->version >= 13) {
		data_put(&reply,
			TAG_U64, 0,	/* underrun_for */
			TAG_U64, 0,	/* playing_for */
			TAG_INVALID);
	}
	return send_data(client, &reply);
}

static int do_get_record_latency(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t channel;
	struct timeval tv, now;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
			TAG_U32, &channel,
			TAG_TIMEVAL, &tv,
			TAG_INVALID)) < 0)
		return res;

	pw_log_debug(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	gettimeofday(&now, NULL);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_USEC, 0,		/* monitor latency */
		TAG_USEC, 0,		/* source latency + queued */
		TAG_BOOLEAN, true,	/* playing state */
		TAG_TIMEVAL, &tv,
		TAG_TIMEVAL, &now,
		TAG_S64, stream->write_index,
		TAG_S64, stream->read_index,
		TAG_INVALID);

	return send_data(client, &reply);
}

static int do_cork_stream(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	bool cork;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
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

	return reply_simple_ack(client, tag);
}

static int do_flush_trigger_prebuf_stream(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
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

static int do_error_access(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	return reply_error(client, tag, ERR_ACCESS);
}

static int do_set_stream_volume(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;
	struct cvolume volume;

	if ((res = data_get(d,
			TAG_U32, &channel,
			TAG_CVOLUME, &volume,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DO_STREAM_VOLUME channel:%u",
			impl, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	pw_stream_set_control(stream->stream,
			SPA_PROP_channelVolumes, volume.channels, volume.values,
			0);

	return reply_simple_ack(client, tag);
}

static int do_set_stream_mute(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;
	bool mute;
	float val;

	if ((res = data_get(d,
			TAG_U32, &channel,
			TAG_BOOLEAN, &mute,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: DO_SET_STREAM_MUTE channel:%u mute:%u",
			impl, channel, mute);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	val = mute ? 1.0f : 0.0f;

	pw_stream_set_control(stream->stream,
			SPA_PROP_mute, 1, &val,
			0);

	return reply_simple_ack(client, tag);
}

static int do_set_stream_name(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	const char *name = NULL;
	struct spa_dict_item items[1];
	int res;

	if ((res = data_get(d,
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

static int do_update_proplist(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel, mode;
	struct stream *stream;
	struct pw_properties *props;
	int res;

	props = pw_properties_new(NULL, NULL);

	if (command != COMMAND_UPDATE_CLIENT_PROPLIST) {
		if ((res = data_get(d,
				TAG_U32, &channel,
				TAG_INVALID)) < 0)
			goto exit;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info(NAME" %p: %s channel:%d", impl, commands[command].name, channel);

	if ((res = data_get(d,
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

static int do_remove_proplist(struct client *client, uint32_t command, uint32_t tag, struct data *d)
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
		if ((res = data_get(d,
				TAG_U32, &channel,
				TAG_INVALID)) < 0)
			goto exit;
	} else {
		channel = SPA_ID_INVALID;
	}

	pw_log_info(NAME" %p: %s channel:%d", impl, commands[command].name, channel);

	while (true) {
		const char *key;

		if ((res = data_get(d,
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

static int do_get_server_info(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	char name[256];
	struct data reply;
	struct sample_spec ss;
	struct channel_map map;

	pw_log_info(NAME" %p: GET_SERVER_INFO", impl);

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	snprintf(name, sizeof(name)-1, "PulseAudio (on PipeWire %s)", pw_get_library_version());

	spa_zero(ss);
	ss.format = SAMPLE_FLOAT32LE;
	ss.rate = 44100;
	ss.channels = 2;

	spa_zero(map);
	map.channels = 2;
	map.map[0] = 1;
	map.map[1] = 2;

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_STRING, name,
		TAG_STRING, "14.0.0",
		TAG_STRING, pw_get_user_name(),
		TAG_STRING, pw_get_host_name(),
		TAG_SAMPLE_SPEC, &ss,
		TAG_STRING, "output.pipewire",
		TAG_STRING, "input.pipewire",
		TAG_U32, 0,
		TAG_INVALID);

	if (client->version >= 15) {
		data_put(&reply,
			TAG_CHANNEL_MAP, &map,
			TAG_INVALID);
	}
	return send_data(client, &reply);
}

static int do_stat(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;

	pw_log_info(NAME" %p: STAT", impl);

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_U32, 0,
		TAG_U32, 0,
		TAG_U32, 0,
		TAG_U32, 0,
		TAG_U32, 0,
		TAG_INVALID);

	return send_data(client, &reply);
}

static int do_lookup(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	const char *name = NULL;
	struct data reply;
	uint32_t idx = 0;
	int res;

	if ((res = data_get(d,
			TAG_STRING, &name,
			TAG_INVALID)) < 0)
		return res;
	if (name == NULL)
		return -EINVAL;

	pw_log_info(NAME" %p: LOOKUP %s", impl, name);

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_U32, idx,
		TAG_INVALID);

	return send_data(client, &reply);
}

static int do_drain_stream(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
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

static void fill_client_info(struct client *client, struct data *d)
{
	data_put(d,
		TAG_U32, 0,				/* client index */
		TAG_STRING, pw_properties_get(client->props, "application.name"),
		TAG_U32, SPA_ID_INVALID,		/* module */ 
		TAG_STRING, "PipeWire",			/* driver */
		TAG_INVALID);
	if (client->version >= 13) {
		data_put(d,
			TAG_PROPLIST, client->props,
			TAG_INVALID);
	}
}

static int do_get_info(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t idx;
	const char *name = NULL;
	int res;

	if ((res = data_get(d,
			TAG_U32, &idx,
			TAG_INVALID)) < 0)
		return res;

	switch (command) {
	case COMMAND_GET_SINK_INFO:
	case COMMAND_GET_SOURCE_INFO:
	case COMMAND_GET_CARD_INFO:
	case COMMAND_GET_SAMPLE_INFO:
		if ((res = data_get(d,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			return res;
		break;
	}

	pw_log_info(NAME" %p: %s idx:%u name:%s", impl,
			commands[command].name, idx, name);

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);

	switch (command) {
	case COMMAND_GET_CLIENT_INFO:
		fill_client_info(client, &reply);
		break;
	case COMMAND_GET_MODULE_INFO:
	case COMMAND_GET_CARD_INFO:
	case COMMAND_GET_SAMPLE_INFO:
		return reply_error(client, -1, ERR_NOENTITY);
	case COMMAND_GET_SINK_INFO:
	case COMMAND_GET_SOURCE_INFO:
		/* fixme, dummy sink/source */
		break;
	case COMMAND_GET_SINK_INPUT_INFO:
	case COMMAND_GET_SOURCE_OUTPUT_INFO:
		/* fixme add ourselves */
		break;
	default:
		return -ENOTSUP;
	}
	return send_data(client, &reply);
}

static int do_get_info_list(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;

	pw_log_info(NAME" %p: %s", impl, commands[command].name);

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);

	switch (command) {
	case COMMAND_GET_CLIENT_INFO_LIST:
		fill_client_info(client, &reply);
		break;
	case COMMAND_GET_MODULE_INFO_LIST:
	case COMMAND_GET_CARD_INFO_LIST:
	case COMMAND_GET_SAMPLE_INFO_LIST:
		break;
	case COMMAND_GET_SINK_INFO_LIST:
	case COMMAND_GET_SOURCE_INFO_LIST:
		/* fixme, dummy sink/source */
		break;
	case COMMAND_GET_SINK_INPUT_INFO_LIST:
	case COMMAND_GET_SOURCE_OUTPUT_INFO_LIST:
		/* fixme add ourselves */
		break;
	default:
		return -ENOTSUP;
	}
	return send_data(client, &reply);
}

static int do_set_stream_buffer_attr(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	uint32_t channel;
	struct stream *stream;
	struct data reply;
	struct buffer_attr attr;
	int res;
	bool adjust_latency = false, early_requests = false;

	if ((res = data_get(d,
			TAG_U32, &channel,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: %s channel:%u", impl, commands[command].name, channel);
	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL) {
		res = -EINVAL;
		return res;
	}

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);

	if (command == COMMAND_SET_PLAYBACK_STREAM_BUFFER_ATTR) {
		if ((res = data_get(d,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.tlength,
				TAG_U32, &attr.prebuf,
				TAG_U32, &attr.minreq,
				TAG_INVALID)) < 0)
			return res;
		if (client->version >= 13) {
			if ((res = data_get(d,
					TAG_BOOLEAN, &adjust_latency,
					TAG_INVALID)) < 0)
				return res;
		}
		if (client->version >= 14) {
			if ((res = data_get(d,
					TAG_BOOLEAN, &early_requests,
					TAG_INVALID)) < 0)
				return res;
		}

		data_put(&reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.tlength,
			TAG_U32, stream->attr.prebuf,
			TAG_U32, stream->attr.minreq,
			TAG_INVALID);
		if (client->version >= 13) {
			data_put(&reply,
				TAG_USEC, 0,		/* configured_sink_latency */
				TAG_INVALID);
		}
	} else {
		if ((res = data_get(d,
				TAG_U32, &attr.maxlength,
				TAG_U32, &attr.fragsize,
				TAG_INVALID)) < 0)
			return res;
		if (client->version >= 13) {
			if ((res = data_get(d,
					TAG_BOOLEAN, &adjust_latency,
					TAG_INVALID)) < 0)
				return res;
		}
		if (client->version >= 14) {
			if ((res = data_get(d,
					TAG_BOOLEAN, &early_requests,
					TAG_INVALID)) < 0)
				return res;
		}
		data_put(&reply,
			TAG_U32, stream->attr.maxlength,
			TAG_U32, stream->attr.fragsize,
			TAG_INVALID);
		if (client->version >= 13) {
			data_put(&reply,
				TAG_USEC, 0,		/* configured_source_latency */
				TAG_INVALID);
		}
	}
	return send_data(client, &reply);
}

static int do_update_stream_sample_rate(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint32_t channel, rate;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
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

	pw_log_info(NAME" %p: client %p free", impl, client);
	spa_list_remove(&client->link);
	pw_map_clear(&client->streams);
	if (client->core)
		pw_core_disconnect(client->core);
	if (client->props)
		pw_properties_free(client->props);
	if (client->source)
		pw_loop_destroy_source(impl->loop, client->source);
	free(client);
}

static int handle_packet(struct client *client)
{
	struct impl *impl = client->impl;
	int res = 0;
	uint32_t command, tag;
	struct data *d = &client->data;

	if (data_get(d,
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

	res = commands[command].run(client, command, tag, d);

finish:
	return res;
}

static int handle_memblock(struct client *client)
{
	struct impl *impl = client->impl;
	struct stream *stream;
	struct block *block;
	uint32_t channel, flags;
	int64_t offset;

	channel = ntohl(client->desc.channel);
	offset = (int64_t) (
             (((uint64_t) ntohl(client->desc.offset_hi)) << 32) |
             (((uint64_t) ntohl(client->desc.offset_lo))));
	flags = ntohl(client->desc.flags) & FLAG_SEEKMASK,

	pw_log_debug(NAME" %p: Received memblock channel:%d offset:%"PRIi64
			" flags:%08x size:%u", impl, channel, offset,
			flags, client->data.length);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	block = calloc(1, sizeof(struct block));
	block->data = client->data.data;
	block->length = client->data.length;
	pw_log_debug("new block %p %p", block, block->data);
	client->data.data = NULL;
	spa_list_append(&stream->blocks, &block->link);
	stream->write_index += block->length;

	return 0;
}

static int do_read(struct client *client)
{
	struct impl *impl = client->impl;
	void *data;
	size_t size;
	ssize_t r;
	int res = 0;

	if (client->index < sizeof(client->desc)) {
		data = SPA_MEMBER(&client->desc, client->index, void);
		size = sizeof(client->desc) - client->index;
	} else {
		uint32_t idx = client->index - sizeof(client->desc);

		if (client->data.data == NULL) {
			res = -EIO;
			goto error;
		}
		data = SPA_MEMBER(client->data.data, idx, void);
		size = client->data.length - idx;
	}
	while (true) {
		if ((r = recv(client->source->fd, data, size, 0)) < 0) {
			if (errno == EINTR)
		                continue;
			res = -errno;
			goto error;
		}
		client->index += r;
		break;
	}

	if (client->index == sizeof(client->desc)) {
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
			client->type = TYPE_PACKET;
		} else {
			client->type = TYPE_MEMBLOCK;
		}
		client->data.data = calloc(1, length);
		client->data.length = length;
		client->data.offset = 0;
	} else if (client->index >= client->data.length + sizeof(client->desc)) {
		switch (client->type) {
		case TYPE_PACKET:
			res = handle_packet(client);
			break;
		case TYPE_MEMBLOCK:
			res = handle_memblock(client);
			break;
		default:
			res = -EPROTO;
			break;
		}
		client->index = 0;
		free(client->data.data);
		client->data.data = NULL;
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

	sm_media_session_add_listener(impl->session,
			&impl->listener,
			&session_events, impl);

	if ((res = create_server(impl, "native")) < 0)
		return res;

	return 0;
}
