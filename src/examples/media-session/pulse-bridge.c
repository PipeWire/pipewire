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

static const uint32_t audio_formats[] = {
	[SAMPLE_U8] = SPA_AUDIO_FORMAT_U8,
	[SAMPLE_ALAW] = SPA_AUDIO_FORMAT_UNKNOWN,
	[SAMPLE_ULAW] = SPA_AUDIO_FORMAT_UNKNOWN,
	[SAMPLE_S16LE] = SPA_AUDIO_FORMAT_S16_LE,
	[SAMPLE_S16BE] = SPA_AUDIO_FORMAT_S16_BE,
	[SAMPLE_FLOAT32LE] = SPA_AUDIO_FORMAT_F32_LE,
	[SAMPLE_FLOAT32BE] = SPA_AUDIO_FORMAT_F32_BE,
	[SAMPLE_S32LE] = SPA_AUDIO_FORMAT_S32_LE,
	[SAMPLE_S32BE] = SPA_AUDIO_FORMAT_S32_BE,
	[SAMPLE_S24LE] = SPA_AUDIO_FORMAT_S24_LE,
	[SAMPLE_S24BE] = SPA_AUDIO_FORMAT_S24_BE,
	[SAMPLE_S24_32LE] = SPA_AUDIO_FORMAT_S24_32_LE,
	[SAMPLE_S24_32BE] = SPA_AUDIO_FORMAT_S24_32_BE,
};

static inline uint32_t format_pa2id(enum sample_format format)
{
	if (format < 0 || (size_t)format >= SPA_N_ELEMENTS(audio_formats))
		return SPA_AUDIO_FORMAT_UNKNOWN;
	return audio_formats[format];
}

struct sample_spec {
	enum sample_format format;
	uint32_t rate;
	uint8_t channels;
};

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

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_list blocks;
	int64_t read_index;
	int64_t write_index;

	struct sample_spec ss;
	struct channel_map map;
	struct buffer_attr attr;

	uint32_t drain_tag;
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
	int (*run) (struct client *client, uint32_t command, uint32_t tag, struct data *d);
};

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
			write_u32(d, l);
			write_arbitrary(d, it->value, l);
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
	write(client->source->fd, &desc, sizeof(desc));
	write(client->source->fd, d->data, d->offset);
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
	struct pw_properties *props;
	int res;

	props = pw_properties_new(NULL, NULL);

	if (client->version < 13) {
		if ((res = data_get(d,
				TAG_STRING, &name,
				TAG_INVALID)) < 0)
			return res;
	} else {
		if ((res = data_get(d,
				TAG_PROPLIST, props,
				TAG_INVALID)) < 0)
			return res;
	}
	if (name)
		pw_properties_set(props, "application.name", name);

	pw_log_info(NAME" %p: SET_CLIENT_NAME %s", impl,
			pw_properties_get(props, "application.name"));

	pw_properties_free(props);

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

static void stream_free(struct stream *stream)
{
	struct client *client = stream->client;
	pw_map_remove(&client->streams, stream->channel);
	if (stream->stream) {
		spa_hook_remove(&stream->stream_listener);
		pw_stream_destroy(stream->stream);
	}
	free(stream);
}

static int send_request(struct stream *stream, uint32_t size)
{
	struct client *client = stream->client;
	uint8_t buffer[1024];
	struct data msg;

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

static int reply_simple_ack(struct stream *stream, uint32_t tag)
{
	struct client *client = stream->client;
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_INVALID);

	return send_data(client, &reply);
}

static int reply_error(struct stream *stream, uint32_t tag, uint32_t error)
{
	struct client *client = stream->client;
	uint8_t buffer[1024];
	struct data reply;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_ERROR,
		TAG_U32, tag,
		TAG_U32, error,
		TAG_INVALID);

	return send_data(client, &reply);
}

static int reply_create_playback_stream(struct stream *stream)
{
	struct impl *impl = stream->impl;
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
		TAG_U32, 0,			/* sink_input/stream index */
		TAG_U32, 8192,			/* missing/requested bytes */
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

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct stream *stream = data;

	switch (state) {
	case PW_STREAM_STATE_ERROR:
		reply_error(stream, 0, 0);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		reply_error(stream, 0, 0);
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

	if (stream->create_tag != SPA_ID_INVALID)
		reply_create_playback_stream(stream);
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

	if (spa_list_is_empty(&stream->blocks))
		return;

	block = spa_list_first(&stream->blocks, struct block, link);

	buffer = pw_stream_dequeue_buffer(stream->stream);
	if (buffer == NULL)
		return;

        buf = buffer->buffer;
        if ((p = buf->datas[0].data) == NULL)
                return;

	maxsize = buf->datas[0].maxsize;
	size = SPA_MIN(block->length, maxsize);
	pw_log_trace("process block %p %p", block, block->data);
	memcpy(p, block->data, size);

	spa_list_remove(&block->link);
	free(block->data);
	free(block);

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = 4;
        buf->datas[0].chunk->size = size;

	pw_stream_queue_buffer(stream->stream, buffer);

	send_request(stream, maxsize);
}

static void stream_drained(void *data)
{
	struct stream *stream = data;
	pw_log_info(NAME" %p: drain", stream);
	reply_simple_ack(stream, stream->drain_tag);
}

static const struct pw_stream_events stream_events =
{
	PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.process = stream_process,
	.drained = stream_drained,
};



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
			PW_DIRECTION_OUTPUT,
			SPA_ID_INVALID,
			PW_STREAM_FLAG_INACTIVE |
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

static int do_get_playback_latency(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t idx;
	struct timeval tv, now;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
			TAG_U32, &idx,
			TAG_TIMEVAL, &tv,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: GET_PLAYBACK_LATENCY idx:%u", impl, idx);
	stream = pw_map_lookup(&client->streams, idx);
	if (stream == NULL)
		return -EINVAL;

	spa_zero(reply);
	reply.data = buffer;
	reply.length = sizeof(buffer);

	data_put(&reply,
		TAG_U32, COMMAND_REPLY,
		TAG_U32, tag,
		TAG_USEC, 0,
		TAG_USEC, 0,
		TAG_BOOLEAN, true,
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

static int do_cork_playback_stream(struct client *client, uint32_t command, uint32_t tag, struct data *d)
{
	struct impl *impl = client->impl;
	uint8_t buffer[1024];
	struct data reply;
	uint32_t idx;
	bool cork;
	struct stream *stream;
	int res;

	if ((res = data_get(d,
			TAG_U32, &idx,
			TAG_BOOLEAN, &cork,
			TAG_INVALID)) < 0)
		return res;

	pw_log_info(NAME" %p: CORK_PLAYBACK_STREAM idx:%u cork:%s",
			impl, idx, cork ? "yes" : "no");
	stream = pw_map_lookup(&client->streams, idx);
	if (stream == NULL)
		return -EINVAL;

//	pw_stream_set_active(stream->stream, !cork);

	reply_simple_ack(stream, tag);
}

static const struct command commands[COMMAND_MAX] =
{
	[COMMAND_AUTH] = { do_command_auth, },
	[COMMAND_SET_CLIENT_NAME] = { do_set_client_name, },
	[COMMAND_SUBSCRIBE] = { do_subscribe, },
	[COMMAND_CREATE_PLAYBACK_STREAM] = { do_create_playback_stream, },
	[COMMAND_GET_PLAYBACK_LATENCY] = { do_get_playback_latency, },
	[COMMAND_CORK_PLAYBACK_STREAM] = { do_cork_playback_stream, },
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

	if (command >= COMMAND_MAX || commands[command].run == NULL) {
		pw_log_error(NAME" %p: command %d not implemented",
				impl, command);
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
	int64_t offset;
	uint32_t channel, flags;
	struct stream *stream;
	struct block *block;

	channel = ntohl(client->desc.channel);
	offset = (int64_t) (
             (((uint64_t) ntohl(client->desc.offset_hi)) << 32) |
             (((uint64_t) ntohl(client->desc.offset_lo))));
	flags = ntohl(client->desc.flags) & FLAG_SEEKMASK,

	pw_log_debug(NAME" %p: Received memblock channel:%d size:%u",
			impl, channel, client->data.length);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL)
		return -EINVAL;

	block = calloc(1, sizeof(struct block));
	block->data = client->data.data;
	block->length = client->data.length;
	pw_log_debug("new block %p %p", block, block->data);
	client->data.data = NULL;
	spa_list_append(&stream->blocks, &block->link);

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
