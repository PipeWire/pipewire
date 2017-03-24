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

#include <spa/type-map.h>
#include <spa/clock.h>
#include <spa/log.h>
#include <spa/list.h>
#include <spa/node.h>
#include <spa/loop.h>
#include <spa/ringbuffer.h>
#include <spa/audio/format-utils.h>
#include <spa/format-builder.h>

typedef struct _SpaALSAState SpaALSAState;
typedef struct _SpaALSABuffer SpaALSABuffer;

typedef struct {
  char device[64];
  char device_name[128];
  char card_name[128];
  uint32_t period_size;
  uint32_t periods;
  bool period_event;
} SpaALSAProps;

#define MAX_BUFFERS 64

struct _SpaALSABuffer {
  SpaBuffer *outbuf;
  SpaMetaHeader *h;
  SpaMetaRingbuffer *rb;
  bool outstanding;
  SpaList link;
};

typedef struct {
  uint32_t node;
  uint32_t clock;
  uint32_t format;
  uint32_t props;
  uint32_t prop_device;
  uint32_t prop_device_name;
  uint32_t prop_card_name;
  uint32_t prop_period_size;
  uint32_t prop_periods;
  uint32_t prop_period_event;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeMediaSubtypeAudio media_subtype_audio;
  SpaTypePropAudio prop_audio;
  SpaTypeAudioFormat audio_format;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeAllocParamBuffers alloc_param_buffers;
  SpaTypeAllocParamMetaEnable alloc_param_meta_enable;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->clock = spa_type_map_get_id (map, SPA_TYPE__Clock);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_device = spa_type_map_get_id (map, SPA_TYPE_PROPS__device);
  type->prop_device_name = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceName);
  type->prop_card_name = spa_type_map_get_id (map, SPA_TYPE_PROPS__cardName);
  type->prop_period_size = spa_type_map_get_id (map, SPA_TYPE_PROPS__periodSize);
  type->prop_periods = spa_type_map_get_id (map, SPA_TYPE_PROPS__periods);
  type->prop_period_event = spa_type_map_get_id (map, SPA_TYPE_PROPS__periodEvent);

  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_media_subtype_audio_map (map, &type->media_subtype_audio);
  spa_type_prop_audio_map (map, &type->prop_audio);
  spa_type_audio_format_map (map, &type->audio_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_alloc_param_buffers_map (map, &type->alloc_param_buffers);
  spa_type_alloc_param_meta_enable_map (map, &type->alloc_param_meta_enable);
}

struct _SpaALSAState {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  uint32_t seq;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop *main_loop;
  SpaLoop *data_loop;

  snd_pcm_stream_t stream;
  snd_output_t *output;

  SpaEventNodeCallback event_cb;
  void *user_data;

  uint8_t props_buffer[1024];
  SpaALSAProps props;

  bool opened;
  snd_pcm_t *hndl;

  bool have_format;
  SpaAudioInfo current_format;
  uint8_t format_buffer[1024];

  snd_pcm_sframes_t buffer_frames;
  snd_pcm_sframes_t period_frames;
  snd_pcm_format_t format;
  int rate;
  int channels;
  size_t frame_size;

  SpaPortInfo info;
  SpaAllocParam *params[3];
  uint8_t params_buffer[1024];
  void *io;

  SpaALSABuffer buffers[MAX_BUFFERS];
  unsigned int n_buffers;
  bool use_ringbuffer;
  SpaALSABuffer *ringbuffer;

  SpaList free;
  SpaList ready;
  size_t ready_offset;

  bool started;
  int n_fds;
  struct pollfd fds[16];
  SpaSource sources[16];

  int64_t sample_count;
  int64_t last_ticks;
  int64_t last_monotonic;
};

int spa_alsa_set_format (SpaALSAState *state,
                         SpaAudioInfo *info,
                         SpaPortFormatFlags flags);

SpaResult spa_alsa_start (SpaALSAState *state, bool xrun_recover);
SpaResult spa_alsa_pause (SpaALSAState *state, bool xrun_recover);
SpaResult spa_alsa_close (SpaALSAState *state);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_ALSA_UTILS_H__ */
