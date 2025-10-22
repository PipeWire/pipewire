/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <arpa/inet.h>
#include <math.h>

#include <spa/debug/buffer.h>
#include <spa/utils/defs.h>
#include <spa/utils/string.h>
#include <spa/debug/log.h>
#include <pipewire/log.h>

#include "commands.h"
#include "defs.h"
#include "format.h"
#include "internal.h"
#include "message.h"
#include "remap.h"
#include "volume.h"

#define MAX_SIZE	(256*1024)
#define MAX_ALLOCATED	(16*1024 *1024)

#define VOLUME_MUTED ((uint32_t) 0U)
#define VOLUME_NORM ((uint32_t) 0x10000U)
#define VOLUME_MAX ((uint32_t) UINT32_MAX/2)

#define PA_CHANNELS_MAX	(32u)

PW_LOG_TOPIC_EXTERN(pulse_conn);
#define PW_LOG_TOPIC_DEFAULT pulse_conn

static inline uint32_t volume_from_linear(float vol)
{
	uint32_t v;
	if (vol <= 0.0f)
		v = VOLUME_MUTED;
	else
		v = SPA_CLAMP((uint64_t) lround(cbrt(vol) * VOLUME_NORM),
				VOLUME_MUTED, VOLUME_MAX);
	return v;
}

static inline float volume_to_linear(uint32_t vol)
{
	float v = ((float)vol) / VOLUME_NORM;
	return v * v * v;
}

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
	ss->format = format_pa2id(tmp);
	if ((res = read_u8(m, &ss->channels)) < 0)
		return res;
	return read_u32(m, &ss->rate);
}

static int read_props(struct message *m, struct pw_properties *props, bool remap)
{
	int res;

	while (true) {
		const char *key;
		const void *data;
		uint32_t length;
		size_t size;
		const struct str_map *map;

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
				TAG_ARBITRARY, &data, &size,
				TAG_INVALID)) < 0)
			return res;

		if (length != size)
			return -EINVAL;

		if (strnlen(data, size) != size - 1)
			continue;

		if (remap && (map = str_map_find(props_key_map, NULL, key)) != NULL) {
			key = map->pw_str;
			if (map->child != NULL &&
			    (map = str_map_find(map->child, NULL, data)) != NULL)
				data = map->pw_str;
		}
		pw_properties_set(props, key, data);
	}
	return 0;
}

static int read_arbitrary(struct message *m, const void **val, size_t *length)
{
	uint32_t len;
	int res;
	if ((res = read_u32(m, &len)) < 0)
		return res;
	if (m->offset + len > m->length)
		return -ENOSPC;
	*val = m->data + m->offset;
	m->offset += len;
	if (length)
		*length = len;
	return 0;
}

static int read_string(struct message *m, char **str)
{
	uint32_t n, maxlen;
	if (m->offset + 1 > m->length)
		return -ENOSPC;
	maxlen = m->length - m->offset;
	n = strnlen(SPA_PTROFF(m->data, m->offset, char), maxlen);
	if (n == maxlen)
		return -EINVAL;
	*str = SPA_PTROFF(m->data, m->offset, char);
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
		map->map[i] = channel_pa2id(tmp);
	}
	return 0;
}
static int read_volume(struct message *m, float *vol)
{
	int res;
	uint32_t v;
	if ((res = read_u32(m, &v)) < 0)
		return res;
	*vol = volume_to_linear(v);
	return 0;
}

static int read_cvolume(struct message *m, struct volume *vol)
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

	spa_zero(*info);
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
	if ((res = read_props(m, info->props, false)) < 0)
		format_info_clear(info);
	return res;
}

int message_get(struct message *m, ...)
{
	va_list va;
	int res = 0;

	va_start(va, m);

	while (true) {
		int tag = va_arg(va, int);
		uint8_t dtag;
		if (tag == TAG_INVALID)
			break;

		if ((res = read_u8(m, &dtag)) < 0)
			goto done;

		switch (dtag) {
		case TAG_STRING:
			if (tag != TAG_STRING)
				goto invalid;
			if ((res = read_string(m, va_arg(va, char**))) < 0)
				goto done;
			break;
		case TAG_STRING_NULL:
			if (tag != TAG_STRING)
				goto invalid;
			*va_arg(va, char**) = NULL;
			break;
		case TAG_U8:
			if (dtag != tag)
				goto invalid;
			if ((res = read_u8(m, va_arg(va, uint8_t*))) < 0)
				goto done;
			break;
		case TAG_U32:
			if (dtag != tag)
				goto invalid;
			if ((res = read_u32(m, va_arg(va, uint32_t*))) < 0)
				goto done;
			break;
		case TAG_S64:
		case TAG_U64:
		case TAG_USEC:
			if (dtag != tag)
				goto invalid;
			if ((res = read_u64(m, va_arg(va, uint64_t*))) < 0)
				goto done;
			break;
		case TAG_SAMPLE_SPEC:
			if (dtag != tag)
				goto invalid;
			if ((res = read_sample_spec(m, va_arg(va, struct sample_spec*))) < 0)
				goto done;
			break;
		case TAG_ARBITRARY:
		{
			const void **val = va_arg(va, const void**);
			size_t *len = va_arg(va, size_t*);
			if (dtag != tag)
				goto invalid;
			if ((res = read_arbitrary(m, val, len)) < 0)
				goto done;
			break;
		}
		case TAG_BOOLEAN_TRUE:
			if (tag != TAG_BOOLEAN)
				goto invalid;
			*va_arg(va, bool*) = true;
			break;
		case TAG_BOOLEAN_FALSE:
			if (tag != TAG_BOOLEAN)
				goto invalid;
			*va_arg(va, bool*) = false;
			break;
		case TAG_TIMEVAL:
			if (dtag != tag)
				goto invalid;
			if ((res = read_timeval(m, va_arg(va, struct timeval*))) < 0)
				goto done;
			break;
		case TAG_CHANNEL_MAP:
			if (dtag != tag)
				goto invalid;
			if ((res = read_channel_map(m, va_arg(va, struct channel_map*))) < 0)
				goto done;
			break;
		case TAG_CVOLUME:
			if (dtag != tag)
				goto invalid;
			if ((res = read_cvolume(m, va_arg(va, struct volume*))) < 0)
				goto done;
			break;
		case TAG_PROPLIST:
			if (dtag != tag)
				goto invalid;
			if ((res = read_props(m, va_arg(va, struct pw_properties*), true)) < 0)
				goto done;
			break;
		case TAG_VOLUME:
			if (dtag != tag)
				goto invalid;
			if ((res = read_volume(m, va_arg(va, float*))) < 0)
				goto done;
			break;
		case TAG_FORMAT_INFO:
			if (dtag != tag)
				goto invalid;
			if ((res = read_format_info(m, va_arg(va, struct format_info*))) < 0)
				goto done;
			break;
		}
	}
	res = 0;
	goto done;

invalid:
	res = -EINVAL;

done:
	va_end(va);

	return res;
}

static int ensure_size(struct message *m, uint32_t size)
{
	uint32_t alloc, diff;
	void *data;

	if (m->length > m->allocated)
		return -ENOMEM;

	if (m->length + size <= m->allocated)
		return size;

	alloc = SPA_ROUND_UP_N(SPA_MAX(m->allocated + size, 4096u), 4096u);
	diff = alloc - m->allocated;
	if ((data = realloc(m->data, alloc)) == NULL) {
		free(m->data);
		m->data = NULL;
		m->impl->stat.allocated -= m->allocated;
		m->allocated = 0;
		return -errno;
	}
	m->impl->stat.allocated += diff;
	m->impl->stat.accumulated += diff;
	m->data = data;
	m->allocated = alloc;
	return size;
}

static void write_8(struct message *m, uint8_t val)
{
	if (ensure_size(m, 1) > 0)
		m->data[m->length] = val;
	m->length++;
}

static void write_32(struct message *m, uint32_t val)
{
	val = htonl(val);
	if (ensure_size(m, 4) > 0)
		memcpy(m->data + m->length, &val, 4);
	m->length += 4;
}

static void write_string(struct message *m, const char *s)
{
	write_8(m, s ? TAG_STRING : TAG_STRING_NULL);
	if (s != NULL) {
		size_t len = strlen(s) + 1;
		if (ensure_size(m, len) > 0)
			memcpy(SPA_PTROFF(m->data, m->length, char), s, len);
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
	uint32_t channels = SPA_MIN(ss->channels, PA_CHANNELS_MAX);
	write_8(m, TAG_SAMPLE_SPEC);
	write_8(m, format_id2pa(ss->format));
	write_8(m, channels);
	write_32(m, ss->rate);
}

static void write_arbitrary(struct message *m, const void *p, size_t length)
{
	write_8(m, TAG_ARBITRARY);
	write_32(m, length);
	if (ensure_size(m, length) > 0)
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
	uint32_t aux = 0, channels = SPA_MIN(map->channels, PA_CHANNELS_MAX);
	write_8(m, TAG_CHANNEL_MAP);
	write_8(m, channels);
	for (i = 0; i < channels; i ++)
		write_8(m, channel_id2pa(map->map[i], &aux));
}

static void write_volume(struct message *m, float vol)
{
	write_8(m, TAG_VOLUME);
	write_32(m, volume_from_linear(vol));
}

static void write_cvolume(struct message *m, struct volume *vol)
{
	uint8_t i;
	uint32_t channels = SPA_MIN(vol->channels, PA_CHANNELS_MAX);
	write_8(m, TAG_CVOLUME);
	write_8(m, channels);
	for (i = 0; i < channels; i ++)
		write_32(m, volume_from_linear(vol->values[i]));
}

static void add_stream_group(struct message *m, struct spa_dict *dict, const char *key,
		const char *media_class, const char *media_role)
{
	const char *str, *id, *prefix;
	char *b;
	int l;

	if (media_class == NULL)
		return;
	if (spa_streq(media_class, "Stream/Output/Audio"))
		prefix = "sink-input";
	else if (spa_streq(media_class, "Stream/Input/Audio"))
		prefix = "source-output";
	else
		return;

	if ((str = media_role) != NULL)
		id = "media-role";
	else if ((str = spa_dict_lookup(dict, PW_KEY_APP_ID)) != NULL)
		id = "application-id";
	else if ((str = spa_dict_lookup(dict, PW_KEY_APP_NAME)) != NULL)
		id = "application-name";
	else if ((str = spa_dict_lookup(dict, PW_KEY_MEDIA_NAME)) != NULL)
		id = "media-name";
	else
		return;

	write_string(m, key);
	l = strlen(prefix) + strlen(id) + strlen(str) + 6; /* "-by-" , ":" and \0 */
	b = alloca(l);
	snprintf(b, l, "%s-by-%s:%s", prefix, id, str);
	write_u32(m, l);
	write_arbitrary(m, b, l);
}

static void write_dict(struct message *m, struct spa_dict *dict, bool remap)
{
	const struct spa_dict_item *it;

	write_8(m, TAG_PROPLIST);
	if (dict != NULL) {
		const char *media_class = NULL, *media_role = NULL;
		spa_dict_for_each(it, dict) {
			const char *key = it->key;
			const char *val = it->value;
			int l;
			const struct str_map *map;

			if (remap && (map = str_map_find(props_key_map, key, NULL)) != NULL) {
				key = map->pa_str;
				if (map->child != NULL &&
				    (map = str_map_find(map->child, val, NULL)) != NULL)
					val = map->pa_str;
			}
			if (spa_streq(key, "media.class"))
				media_class = val;
			if (spa_streq(key, "media.role"))
				media_role = val;

			write_string(m, key);
			l = strlen(val) + 1;
			write_u32(m, l);
			write_arbitrary(m, val, l);

		}
		if (remap)
			add_stream_group(m, dict, "module-stream-restore.id",
					media_class, media_role);
	}
	write_string(m, NULL);
}

static void write_format_info(struct message *m, struct format_info *info)
{
	write_8(m, TAG_FORMAT_INFO);
	write_u8(m, (uint8_t) info->encoding);
	write_dict(m, info->props ? &info->props->dict : NULL, false);
}

int message_put(struct message *m, ...)
{
	va_list va;

	if (m == NULL)
		return -EINVAL;

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
			write_cvolume(m, va_arg(va, struct volume*));
			break;
		case TAG_PROPLIST:
			write_dict(m, va_arg(va, struct spa_dict*), true);
			break;
		case TAG_VOLUME:
			write_volume(m, (float)va_arg(va, double));
			break;
		case TAG_FORMAT_INFO:
			write_format_info(m, va_arg(va, struct format_info*));
			break;
		}
	}
	va_end(va);

	if (m->length > m->allocated)
		return -ENOMEM;

	return 0;
}

int message_dump(enum spa_log_level level, const char *prefix, struct message *m)
{
	int res;
	uint32_t i, offset = m->offset, o;

	m->offset = 0;

	pw_log(level, "%s message: len:%d alloc:%u", prefix, m->length, m->allocated);
	while (true) {
		uint8_t tag;

		o = m->offset;
		if (read_u8(m, &tag) < 0)
			break;

		switch (tag) {
		case TAG_STRING:
		{
			char *val;
			if ((res = read_string(m, &val)) < 0)
				return res;
			pw_log(level, "%s %u: string: '%s'", prefix, o, val);
			break;
			}
		case TAG_STRING_NULL:
			pw_log(level, "%s %u: string: NULL", prefix, o);
			break;
		case TAG_U8:
		{
			uint8_t val;
			if ((res = read_u8(m, &val)) < 0)
				return res;
			pw_log(level, "%s %u: u8: %u", prefix, o, val);
			break;
		}
		case TAG_U32:
		{
			uint32_t val;
			if ((res = read_u32(m, &val)) < 0)
				return res;
			if (o == 0) {
				pw_log(level, "%s %u: u32: %u (command %s)",
						prefix, o, val,
						val < COMMAND_MAX ? commands[val].name : "INVALID");
			} else {
				pw_log(level, "%s %u: u32: %u", prefix, o, val);
			}
			break;
		}
		case TAG_S64:
		{
			uint64_t val;
			if ((res = read_u64(m, &val)) < 0)
				return res;
			pw_log(level, "%s %u: s64: %"PRIi64"", prefix, o, (int64_t)val);
			break;
		}
		case TAG_U64:
		{
			uint64_t val;
			if ((res = read_u64(m, &val)) < 0)
				return res;
			pw_log(level, "%s %u: u64: %"PRIu64"", prefix, o, val);
			break;
		}
		case TAG_USEC:
		{
			uint64_t val;
			if ((res = read_u64(m, &val)) < 0)
				return res;
			pw_log(level, "%s %u: u64: %"PRIu64"", prefix, o, val);
			break;
		}
		case TAG_SAMPLE_SPEC:
		{
			struct sample_spec ss;
			if ((res = read_sample_spec(m, &ss)) < 0)
				return res;
			pw_log(level, "%s %u: ss: format:%s rate:%d channels:%u", prefix, o,
					format_id2name(ss.format), ss.rate,
					ss.channels);
			break;
		}
		case TAG_ARBITRARY:
		{
			const void *mem;
			size_t len;
			if ((res = read_arbitrary(m, &mem, &len)) < 0)
				return res;
			spa_debug_log_mem(pw_log_get(), level, 0, mem, len);
			break;
		}
		case TAG_BOOLEAN_TRUE:
			pw_log(level, "%s %u: bool: true", prefix, o);
			break;
		case TAG_BOOLEAN_FALSE:
			pw_log(level, "%s %u: bool: false", prefix, o);
			break;
		case TAG_TIMEVAL:
		{
			struct timeval tv;
			if ((res = read_timeval(m, &tv)) < 0)
				return res;
			pw_log(level, "%s %u: timeval: %jd:%jd", prefix, o,
				(intmax_t) tv.tv_sec, (intmax_t) tv.tv_usec);
			break;
		}
		case TAG_CHANNEL_MAP:
		{
			struct channel_map map;
			char pos[8];
			if ((res = read_channel_map(m, &map)) < 0)
				return res;
			pw_log(level, "%s %u: channelmap: channels:%u", prefix, o, map.channels);
			for (i = 0; i < map.channels; i++)
				pw_log(level, "%s     %d: %s", prefix, i,
						channel_id2name(map.map[i], pos, sizeof(pos)));
			break;
		}
		case TAG_CVOLUME:
		{
			struct volume vol;
			if ((res = read_cvolume(m, &vol)) < 0)
				return res;
			pw_log(level, "%s %u: cvolume: channels:%u", prefix, o, vol.channels);
			for (i = 0; i < vol.channels; i++)
				pw_log(level, "%s     %d: %f", prefix, i, vol.values[i]);
			break;
		}
		case TAG_PROPLIST:
		{
			struct pw_properties *props = pw_properties_new(NULL, NULL);
			const struct spa_dict_item *it;
			res = read_props(m, props, false);
			if (res >= 0) {
				pw_log(level, "%s %u: props: n_items:%u", prefix, o, props->dict.n_items);
				spa_dict_for_each(it, &props->dict)
					pw_log(level, "%s      '%s': '%s'", prefix, it->key, it->value);
			}
			pw_properties_free(props);
			if (res < 0)
				return res;
			break;
		}
		case TAG_VOLUME:
		{
			float vol;
			if ((res = read_volume(m, &vol)) < 0)
				return res;
			pw_log(level, "%s %u: volume: %f", prefix, o, vol);
			break;
		}
		case TAG_FORMAT_INFO:
		{
			struct format_info info;
			const struct spa_dict_item *it;
			if ((res = read_format_info(m, &info)) < 0)
				return res;
			pw_log(level, "%s %u: format-info: enc:%s n_items:%u",
					prefix, o, format_encoding2name(info.encoding),
					info.props->dict.n_items);
			spa_dict_for_each(it, &info.props->dict)
				pw_log(level, "%s      '%s': '%s'", prefix, it->key, it->value);
			format_info_clear(&info);
			break;
		}
		}
	}
	m->offset = offset;

	return 0;
}

struct message *message_alloc(struct impl *impl, uint32_t channel, uint32_t size)
{
	struct message *msg;

	if (!spa_list_is_empty(&impl->free_messages)) {
		msg = spa_list_first(&impl->free_messages, struct message, link);
		spa_list_remove(&msg->link);
		pw_log_trace("using recycled message %p size:%d", msg, size);

		spa_assert(msg->impl == impl);
	} else {
		if ((msg = calloc(1, sizeof(*msg))) == NULL)
			return NULL;

		pw_log_trace("new message %p size:%d", msg, size);
		msg->impl = impl;
		msg->impl->stat.n_allocated++;
		msg->impl->stat.n_accumulated++;
	}

	if (ensure_size(msg, size) < 0) {
		message_free(msg, false, true);
		return NULL;
	}

	msg->type = MESSAGE_TYPE_UNSPECIFIED;
	msg->channel = channel;
	msg->offset = 0;
	msg->length = size;

	return msg;
}

void message_free(struct message *msg, bool dequeue, bool destroy)
{
	if (dequeue)
		spa_list_remove(&msg->link);

	if (msg->impl->stat.allocated > MAX_ALLOCATED || msg->allocated > MAX_SIZE)
		destroy = true;

	if (destroy) {
		pw_log_trace("destroy message %p size:%d", msg, msg->allocated);
		msg->impl->stat.n_allocated--;
		msg->impl->stat.allocated -= msg->allocated;
		free(msg->data);
		free(msg);
	} else {
		pw_log_trace("recycle message %p size:%d/%d", msg, msg->length, msg->allocated);
		spa_list_append(&msg->impl->free_messages, &msg->link);
		msg->length = 0;
	}
}
