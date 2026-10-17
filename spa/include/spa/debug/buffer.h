/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_DEBUG_BUFFER_H
#define SPA_DEBUG_BUFFER_H

#include <inttypes.h>

#include <spa/debug/context.h>
#include <spa/debug/mem.h>
#include <spa/debug/pod.h>
#include <spa/debug/types.h>
#include <spa/buffer/type-info.h>
#include <spa/param/video/type-info.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup spa_debug Debug
 * Debugging utilities
 */

/**
 * \addtogroup spa_debug
 * \{
 */

#ifndef SPA_API_DEBUG_BUFFER
 #ifdef SPA_API_IMPL
  #define SPA_API_DEBUG_BUFFER SPA_API_IMPL
 #else
  #define SPA_API_DEBUG_BUFFER static inline
 #endif
#endif

SPA_API_DEBUG_BUFFER int spa_debugc_buffer(struct spa_debug_context *ctx, int indent, const struct spa_buffer *buffer)
{
	uint32_t i;

	spa_debugc(ctx, "%*s" "struct spa_buffer %p:", indent, "", buffer);
	spa_debugc(ctx, "%*s" " n_metas: %u (at %p)", indent, "", buffer->n_metas, buffer->metas);
	for (i = 0; i < buffer->n_metas; i++) {
		struct spa_meta *m = &buffer->metas[i];
		const char *type_name;

		type_name = spa_debug_type_find_name(spa_type_meta_type, m->type);
		spa_debugc(ctx, "%*s" "  meta %d: type %d (%s), data %p, size %d:", indent, "", i, m->type,
			type_name, m->data, m->size);

		switch (m->type) {
		case SPA_META_Header:
		{
			struct spa_meta_header *h = (struct spa_meta_header*)m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_header:", indent, "");
			spa_debugc(ctx, "%*s" "      flags:      %08x", indent, "", h->flags);
			spa_debugc(ctx, "%*s" "      offset:     %u", indent, "", h->offset);
			spa_debugc(ctx, "%*s" "      seq:        %" PRIu64, indent, "", h->seq);
			spa_debugc(ctx, "%*s" "      pts:        %" PRIi64, indent, "", h->pts);
			spa_debugc(ctx, "%*s" "      dts_offset: %" PRIi64, indent, "", h->dts_offset);
			break;
		}
		case SPA_META_VideoCrop:
		{
			struct spa_meta_region *h = (struct spa_meta_region*)m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_region:", indent, "");
			spa_debugc(ctx, "%*s" "      x:      %d", indent, "", h->region.position.x);
			spa_debugc(ctx, "%*s" "      y:      %d", indent, "", h->region.position.y);
			spa_debugc(ctx, "%*s" "      width:  %d", indent, "", h->region.size.width);
			spa_debugc(ctx, "%*s" "      height: %d", indent, "", h->region.size.height);
			break;
		}
		case SPA_META_VideoDamage:
		{
			struct spa_meta_region *h;
			spa_meta_for_each(h, m) {
				spa_debugc(ctx, "%*s" "    struct spa_meta_region:", indent, "");
				spa_debugc(ctx, "%*s" "      x:      %d", indent, "", h->region.position.x);
				spa_debugc(ctx, "%*s" "      y:      %d", indent, "", h->region.position.y);
				spa_debugc(ctx, "%*s" "      width:  %d", indent, "", h->region.size.width);
				spa_debugc(ctx, "%*s" "      height: %d", indent, "", h->region.size.height);
			}
			break;
		}
		case SPA_META_Bitmap:
		{
			const struct spa_meta_bitmap *h = (const struct spa_meta_bitmap *) m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_bitmap:", indent, "");
			spa_debugc(ctx, "%*s" "      format: %" PRIu32 " (%s)", indent, "",
					h->format, spa_debug_type_find_name(spa_type_video_format, h->format));
			spa_debugc(ctx, "%*s" "      size:", indent, "");
			spa_debugc(ctx, "%*s" "        width: %" PRIu32, indent, "", h->size.width);
			spa_debugc(ctx, "%*s" "        height: %" PRIu32, indent, "", h->size.height);
			spa_debugc(ctx, "%*s" "      stride: %" PRId32, indent, "", h->stride);
			spa_debugc(ctx, "%*s" "      offset: %" PRIu32, indent, "", h->offset);
			break;
		}
		case SPA_META_Cursor:
		{
			const struct spa_meta_cursor *h = (const struct spa_meta_cursor *) m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_cursor:", indent, "");
			spa_debugc(ctx, "%*s" "      id: %" PRIu32, indent, "", h->id);
			spa_debugc(ctx, "%*s" "      flags: %08" PRIx32, indent, "", h->flags);
			spa_debugc(ctx, "%*s" "      position:", indent, "");
			spa_debugc(ctx, "%*s" "        x: %" PRId32, indent, "", h->position.x);
			spa_debugc(ctx, "%*s" "        y: %" PRId32, indent, "", h->position.y);
			spa_debugc(ctx, "%*s" "      hotspot:", indent, "");
			spa_debugc(ctx, "%*s" "        x: %" PRId32, indent, "", h->hotspot.x);
			spa_debugc(ctx, "%*s" "        y: %" PRId32, indent, "", h->hotspot.y);
			spa_debugc(ctx, "%*s" "      bitmap_offset: %" PRIu32, indent, "", h->bitmap_offset);
			break;
		}
		case SPA_META_Control:
		{
			const struct spa_meta_control *h = (const struct spa_meta_control *) m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_control:", indent, "");
			spa_debugc(ctx, "%*s" "      sequence:", indent, "");
			spa_debugc_pod(ctx, indent + 8, NULL, &h->sequence.pod);
			break;
		}
		case SPA_META_Busy:
		{
			const struct spa_meta_busy *h = (const struct spa_meta_busy *) m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_busy:", indent, "");
			spa_debugc(ctx, "%*s" "      flags: %08" PRIx32, indent, "", h->flags);
			spa_debugc(ctx, "%*s" "      count: %" PRIu32, indent, "", SPA_ATOMIC_LOAD(h->count));
			break;
		}
		case SPA_META_VideoTransform:
		{
			const struct spa_meta_videotransform *h = (const struct spa_meta_videotransform *) m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_videotransform:", indent, "");
			spa_debugc(ctx, "%*s" "      transform: %" PRIu32 " (%s)", indent, "",
					h->transform, spa_debug_type_find_name(spa_type_meta_videotransform_type, h->transform));
			break;
		}
		case SPA_META_SyncTimeline:
		{
			const struct spa_meta_sync_timeline *h = (const struct spa_meta_sync_timeline *) m->data;
			if (m->size < sizeof(*h))
				break;

			spa_debugc(ctx, "%*s" "    struct spa_meta_sync_timeline:", indent, "");
			spa_debugc(ctx, "%*s" "      flags: %08" PRIx32, indent, "", h->flags);
			spa_debugc(ctx, "%*s" "      acquire_point: %" PRIu64, indent, "", h->acquire_point);
			spa_debugc(ctx, "%*s" "      release_point: %" PRIu64, indent, "", h->release_point);
			break;
		}
		default:
			spa_debugc(ctx, "%*s" "    Unknown:", indent, "");
			spa_debugc_mem(ctx, 5, m->data, m->size);
		}
	}
	spa_debugc(ctx, "%*s" " n_datas: %u (at %p)", indent, "", buffer->n_datas, buffer->datas);
	for (i = 0; i < buffer->n_datas; i++) {
		struct spa_data *d = &buffer->datas[i];
		spa_debugc(ctx, "%*s" "   type:    %d (%s)", indent, "", d->type,
			spa_debug_type_find_name(spa_type_data_type, d->type));
		spa_debugc(ctx, "%*s" "   flags:   %d", indent, "", d->flags);
		spa_debugc(ctx, "%*s" "   data:    %p", indent, "", d->data);
		spa_debugc(ctx, "%*s" "   fd:      %" PRIi64, indent, "", d->fd);
		spa_debugc(ctx, "%*s" "   offset:  %d", indent, "", d->mapoffset);
		spa_debugc(ctx, "%*s" "   maxsize: %u", indent, "", d->maxsize);
		spa_debugc(ctx, "%*s" "   chunk:   %p", indent, "", d->chunk);
		spa_debugc(ctx, "%*s" "    offset: %d", indent, "", d->chunk->offset);
		spa_debugc(ctx, "%*s" "    size:   %u", indent, "", d->chunk->size);
		spa_debugc(ctx, "%*s" "    stride: %d", indent, "", d->chunk->stride);
	}
	return 0;
}

SPA_API_DEBUG_BUFFER int spa_debug_buffer(int indent, const struct spa_buffer *buffer)
{
	return spa_debugc_buffer(NULL, indent, buffer);
}
/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_DEBUG_BUFFER_H */
