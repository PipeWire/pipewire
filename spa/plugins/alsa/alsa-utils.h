/* Spa ALSA Sink
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

#ifndef __SPA_ALSA_UTILS_H__
#define __SPA_ALSA_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include <asoundlib.h>

#include <spa/support/type-map.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/list.h>

#include <spa/clock/clock.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/audio/format-utils.h>

struct props {
	char device[64];
	char device_name[128];
	char card_name[128];
	uint32_t min_latency;
	uint32_t max_latency;
};

#define MAX_BUFFERS 32

struct buffer {
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
#define BUFFER_FLAG_OUT	(1<<0)
	uint32_t flags;
	struct spa_list link;
};

struct type {
	uint32_t node;
	uint32_t clock;
	uint32_t format;
	uint32_t props;
	uint32_t prop_device;
	uint32_t prop_device_name;
	uint32_t prop_card_name;
	uint32_t prop_min_latency;
	uint32_t prop_max_latency;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_media_subtype_audio media_subtype_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_format_audio format_audio;
	struct spa_type_param_buffers param_buffers;
	struct spa_type_param_meta param_meta;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->clock = spa_type_map_get_id(map, SPA_TYPE__Clock);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->prop_device = spa_type_map_get_id(map, SPA_TYPE_PROPS__device);
	type->prop_device_name = spa_type_map_get_id(map, SPA_TYPE_PROPS__deviceName);
	type->prop_card_name = spa_type_map_get_id(map, SPA_TYPE_PROPS__cardName);
	type->prop_min_latency = spa_type_map_get_id(map, SPA_TYPE_PROPS__minLatency);
	type->prop_max_latency = spa_type_map_get_id(map, SPA_TYPE_PROPS__maxLatency);

	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_media_subtype_audio_map(map, &type->media_subtype_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_param_buffers_map(map, &type->param_buffers);
	spa_type_param_meta_map(map, &type->param_meta);
}

struct state {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_clock clock;

	uint32_t seq;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;

	snd_pcm_stream_t stream;
	snd_output_t *output;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct props props;

	bool opened;
	snd_pcm_t *hndl;

	bool have_format;
	struct spa_audio_info current_format;

	snd_pcm_uframes_t buffer_frames;
	snd_pcm_uframes_t period_frames;
	snd_pcm_format_t format;
	int rate;
	int channels;
	size_t frame_size;

	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_io_control_range *range;

	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;

	struct spa_list free;
	struct spa_list ready;

	size_t ready_offset;

	bool started;
	struct spa_source source;
	int timerfd;
	bool alsa_started;
	int threshold;

	snd_htimestamp_t now;
	int64_t sample_count;
	int64_t filled;
	int64_t last_ticks;
	int64_t last_monotonic;

	uint64_t underrun;
};

int
spa_alsa_enum_format(struct state *state,
		     uint32_t *index,
		     const struct spa_pod *filter,
		     struct spa_pod **result,
		     struct spa_pod_builder *builder);

int spa_alsa_set_format(struct state *state, struct spa_audio_info *info, uint32_t flags);

int spa_alsa_start(struct state *state, bool xrun_recover);
int spa_alsa_pause(struct state *state, bool xrun_recover);
int spa_alsa_close(struct state *state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_ALSA_UTILS_H__ */
