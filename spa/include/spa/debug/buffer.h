/* Simple Plugin API
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_DEBUG_BUFFER_H__
#define __SPA_DEBUG_BUFFER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/debug/debug-mem.h>
#include <spa/buffer/buffer-types.h>

#ifndef spa_debug
#define spa_debug(...)	({ fprintf(stderr, __VA_ARGS__);fputc('\n', stderr); })
#endif

static inline int spa_debug_buffer(int indent, const struct spa_buffer *buffer)
{
	int i;

	spa_debug("%*s" "struct spa_buffer %p:", indent, "", buffer);
	spa_debug("%*s" " id:      %08X", indent, "", buffer->id);
	spa_debug("%*s" " n_metas: %u (at %p)", indent, "", buffer->n_metas, buffer->metas);
	for (i = 0; i < buffer->n_metas; i++) {
		struct spa_meta *m = &buffer->metas[i];
		const char *type_name;

		type_name = spa_debug_type_find_name(spa_type_meta_type, m->type);
		spa_debug("%*s" "  meta %d: type %d (%s), data %p, size %d:", indent, "", i, m->type,
			type_name, m->data, m->size);

		switch (m->type) {
		case SPA_META_Header:
		{
			struct spa_meta_header *h = m->data;
			spa_debug("%*s" "    struct spa_meta_header:", indent, "");
			spa_debug("%*s" "      flags:      %08x", indent, "", h->flags);
			spa_debug("%*s" "      seq:        %u", indent, "", h->seq);
			spa_debug("%*s" "      pts:        %" PRIi64, indent, "", h->pts);
			spa_debug("%*s" "      dts_offset: %" PRIi64, indent, "", h->dts_offset);
			break;
		}
		case SPA_META_VideoCrop:
		{
			struct spa_meta_video_crop *h = m->data;
			spa_debug("%*s" "    struct spa_meta_video_crop:", indent, "");
			spa_debug("%*s" "      x:      %d", indent, "", h->x);
			spa_debug("%*s" "      y:      %d", indent, "", h->y);
			spa_debug("%*s" "      width:  %d", indent, "", h->width);
			spa_debug("%*s" "      height: %d", indent, "", h->height);
			break;
		}
		case SPA_META_VideoDamage:
		default:
			spa_debug("%*s" "    Unknown:", indent, "");
			spa_debug_mem(5, m->data, m->size);
		}
	}
	spa_debug("%*s" " n_datas: \t%u (at %p)", indent, "", buffer->n_datas, buffer->datas);
	for (i = 0; i < buffer->n_datas; i++) {
		struct spa_data *d = &buffer->datas[i];
		spa_debug("%*s" "   type:    %d (%s)", indent, "", d->type,
			spa_debug_type_find_name(spa_type_data_type, d->type));
		spa_debug("%*s" "   flags:   %d", indent, "", d->flags);
		spa_debug("%*s" "   data:    %p", indent, "", d->data);
		spa_debug("%*s" "   fd:      %d", indent, "", d->fd);
		spa_debug("%*s" "   offset:  %d", indent, "", d->mapoffset);
		spa_debug("%*s" "   maxsize: %u", indent, "", d->maxsize);
		spa_debug("%*s" "   chunk:   %p", indent, "", d->chunk);
		spa_debug("%*s" "    offset: %d", indent, "", d->chunk->offset);
		spa_debug("%*s" "    size:   %u", indent, "", d->chunk->size);
		spa_debug("%*s" "    stride: %d", indent, "", d->chunk->stride);
	}
	return 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DEBUG_BUFFER_H__ */
