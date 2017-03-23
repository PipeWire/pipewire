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

#include <spa/id-map.h>
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
  SpaMediaTypes media_types;
  SpaMediaSubtypes media_subtypes;
  SpaMediaSubtypesAudio media_subtypes_audio;
  SpaPropAudio prop_audio;
  SpaAudioFormats audio_formats;
  SpaEventNode event_node;
  SpaCommandNode command_node;
  SpaAllocParamBuffers alloc_param_buffers;
  SpaAllocParamMetaEnable alloc_param_meta_enable;
} URI;

static inline void
init_uri (URI *uri, SpaIDMap *map)
{
  uri->node = spa_id_map_get_id (map, SPA_TYPE__Node);
  uri->clock = spa_id_map_get_id (map, SPA_TYPE__Clock);
  uri->format = spa_id_map_get_id (map, SPA_TYPE__Format);
  uri->props = spa_id_map_get_id (map, SPA_TYPE__Props);
  uri->prop_device = spa_id_map_get_id (map, SPA_TYPE_PROPS__device);
  uri->prop_device_name = spa_id_map_get_id (map, SPA_TYPE_PROPS__deviceName);
  uri->prop_card_name = spa_id_map_get_id (map, SPA_TYPE_PROPS__cardName);
  uri->prop_period_size = spa_id_map_get_id (map, SPA_TYPE_PROPS__periodSize);
  uri->prop_periods = spa_id_map_get_id (map, SPA_TYPE_PROPS__periods);
  uri->prop_period_event = spa_id_map_get_id (map, SPA_TYPE_PROPS__periodEvent);

  spa_media_types_fill (&uri->media_types, map);
  spa_media_subtypes_map (map, &uri->media_subtypes);
  spa_media_subtypes_audio_map (map, &uri->media_subtypes_audio);
  spa_prop_audio_map (map, &uri->prop_audio);
  spa_audio_formats_map (map, &uri->audio_formats);
  spa_event_node_map (map, &uri->event_node);
  spa_command_node_map (map, &uri->command_node);
  spa_alloc_param_buffers_map (map, &uri->alloc_param_buffers);
  spa_alloc_param_meta_enable_map (map, &uri->alloc_param_meta_enable);
}

struct _SpaALSAState {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  uint32_t seq;

  URI uri;
  SpaIDMap *map;
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
