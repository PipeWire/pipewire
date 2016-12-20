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
#include <spa/log.h>
#include <spa/list.h>
#include <spa/node.h>
#include <spa/loop.h>
#include <spa/ringbuffer.h>
#include <spa/audio/format.h>

typedef struct _SpaALSAState SpaALSAState;
typedef struct _SpaALSABuffer SpaALSABuffer;

typedef struct {
  SpaProps props;
  char device[64];
  char device_name[128];
  char card_name[128];
  uint32_t buffer_time;
  uint32_t period_time;
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
} URI;

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

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaALSAProps props[2];

  bool opened;
  snd_pcm_t *hndl;

  bool have_format;
  SpaFormatAudio query_format;
  SpaFormatAudio current_format;
  snd_pcm_sframes_t buffer_frames;
  snd_pcm_sframes_t period_frames;
  snd_pcm_format_t format;
  int rate;
  int channels;
  size_t frame_size;

  SpaPortInfo info;
  SpaAllocParam *params[3];
  SpaAllocParamBuffers param_buffers;
  SpaAllocParamMetaEnable param_meta;
  SpaAllocParamMetaEnableRingbuffer param_meta_rb;
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
                         SpaFormatAudio *fmt,
                         SpaPortFormatFlags flags);

SpaResult spa_alsa_start (SpaALSAState *state, bool xrun_recover);
SpaResult spa_alsa_pause (SpaALSAState *state, bool xrun_recover);
SpaResult spa_alsa_close (SpaALSAState *state);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_ALSA_UTILS_H__ */
