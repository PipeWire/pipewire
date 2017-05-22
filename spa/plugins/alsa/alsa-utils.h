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
#include <spa/param-alloc.h>
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
  uint32_t min_latency;
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
  uint32_t prop_min_latency;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeMediaSubtypeAudio media_subtype_audio;
  SpaTypeFormatAudio format_audio;
  SpaTypeAudioFormat audio_format;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeParamAllocBuffers param_alloc_buffers;
  SpaTypeParamAllocMetaEnable param_alloc_meta_enable;
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
  type->prop_min_latency = spa_type_map_get_id (map, SPA_TYPE_PROPS__minLatency);

  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_media_subtype_audio_map (map, &type->media_subtype_audio);
  spa_type_format_audio_map (map, &type->format_audio);
  spa_type_audio_format_map (map, &type->audio_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_param_alloc_buffers_map (map, &type->param_alloc_buffers);
  spa_type_param_alloc_meta_enable_map (map, &type->param_alloc_meta_enable);
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

  SpaNodeCallbacks callbacks;
  void *user_data;

  uint8_t props_buffer[1024];
  SpaALSAProps props;

  bool opened;
  snd_pcm_t *hndl;

  bool have_format;
  SpaAudioInfo current_format;
  uint8_t format_buffer[1024];

  snd_pcm_uframes_t buffer_frames;
  snd_pcm_uframes_t period_frames;
  snd_pcm_format_t format;
  int rate;
  int channels;
  size_t frame_size;

  SpaPortInfo info;
  uint32_t params[3];
  uint8_t params_buffer[1024];
  SpaPortIO *io;

  SpaALSABuffer buffers[MAX_BUFFERS];
  unsigned int n_buffers;

  SpaList free;
  SpaList ready;
  size_t ready_offset;

  bool started;
  SpaSource source;
  int timerfd;
  bool alsa_started;
  int threshold;

  int64_t sample_count;
  int64_t last_ticks;
  int64_t last_monotonic;
};

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)

SpaResult
spa_alsa_enum_format (SpaALSAState    *state,
                      SpaFormat      **format,
                      const SpaFormat *filter,
                      uint32_t         index);

int spa_alsa_set_format (SpaALSAState *state,
                         SpaAudioInfo *info,
                         uint32_t      flags);

SpaResult spa_alsa_start (SpaALSAState *state, bool xrun_recover);
SpaResult spa_alsa_pause (SpaALSAState *state, bool xrun_recover);
SpaResult spa_alsa_close (SpaALSAState *state);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_ALSA_UTILS_H__ */
