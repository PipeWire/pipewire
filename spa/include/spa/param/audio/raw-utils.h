/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_AUDIO_RAW_UTILS_H__
#define __SPA_AUDIO_RAW_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/type-map.h>
#include <spa/param/audio/raw.h>

#if __BYTE_ORDER == __BIG_ENDIAN
#define _SPA_TYPE_AUDIO_FORMAT_NE(fmt) SPA_TYPE_AUDIO_FORMAT_BASE fmt "BE"
#define _SPA_TYPE_AUDIO_FORMAT_OE(fmt) SPA_TYPE_AUDIO_FORMAT_BASE fmt "LE"
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _SPA_TYPE_AUDIO_FORMAT_NE(fmt) SPA_TYPE_AUDIO_FORMAT_BASE fmt "LE"
#define _SPA_TYPE_AUDIO_FORMAT_OE(fmt) SPA_TYPE_AUDIO_FORMAT_BASE fmt "BE"
#endif

struct spa_type_audio_format {
	uint32_t UNKNOWN;
	uint32_t ENCODED;
	uint32_t S8;
	uint32_t U8;
	uint32_t S16;
	uint32_t U16;
	uint32_t S24_32;
	uint32_t U24_32;
	uint32_t S32;
	uint32_t U32;
	uint32_t S24;
	uint32_t U24;
	uint32_t S20;
	uint32_t U20;
	uint32_t S18;
	uint32_t U18;
	uint32_t F32;
	uint32_t F64;
	uint32_t S16_OE;
	uint32_t U16_OE;
	uint32_t S24_32_OE;
	uint32_t U24_32_OE;
	uint32_t S32_OE;
	uint32_t U32_OE;
	uint32_t S24_OE;
	uint32_t U24_OE;
	uint32_t S20_OE;
	uint32_t U20_OE;
	uint32_t S18_OE;
	uint32_t U18_OE;
	uint32_t F32_OE;
	uint32_t F64_OE;
};

static inline void
spa_type_audio_format_map(struct spa_type_map *map, struct spa_type_audio_format *type)
{
	if (type->ENCODED == 0) {
		type->UNKNOWN = 0;
		type->ENCODED = spa_type_map_get_id(map, SPA_TYPE_AUDIO_FORMAT__ENCODED);

		type->S8 = spa_type_map_get_id(map, SPA_TYPE_AUDIO_FORMAT__S8);
		type->U8 = spa_type_map_get_id(map, SPA_TYPE_AUDIO_FORMAT__U8);

		type->S16 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("S16"));
		type->U16 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("U16"));
		type->S24_32 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("S24_32"));
		type->U24_32 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("U24_32"));
		type->S32 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("S32"));
		type->U32 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("U32"));
		type->S24 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("S24"));
		type->U24 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("U24"));
		type->S20 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("S20"));
		type->U20 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("U20"));
		type->S18 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("S18"));
		type->U18 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("U18"));
		type->F32 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("F32"));
		type->F64 = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_NE("F64"));

		type->S16_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("S16"));
		type->U16_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("U16"));
		type->S24_32_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("S24_32"));
		type->U24_32_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("U24_32"));
		type->S32_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("S32"));
		type->U32_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("U32"));
		type->S24_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("S24"));
		type->U24_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("U24"));
		type->S20_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("S20"));
		type->U20_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("U20"));
		type->S18_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("S18"));
		type->U18_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("U18"));
		type->F32_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("F32"));
		type->F64_OE = spa_type_map_get_id(map, _SPA_TYPE_AUDIO_FORMAT_OE("F64"));
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_RAW_UTILS_H__ */
