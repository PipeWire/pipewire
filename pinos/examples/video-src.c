/* Pinos
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>

#include <spa/type-map.h>
#include <spa/format-utils.h>
#include <spa/video/format-utils.h>
#include <spa/format-builder.h>
#include <spa/props.h>

#include <pinos/client/pinos.h>
#include <pinos/client/sig.h>
#include <spa/lib/debug.h>

typedef struct {
  uint32_t format;
  uint32_t props;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatVideo format_video;
  SpaTypeVideoFormat video_format;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
}

#define BPP    3

typedef struct {
  Type type;

  bool running;
  PinosLoop *loop;
  SpaSource *timer;

  PinosContext *context;
  PinosListener on_state_changed;

  PinosStream *stream;
  PinosListener on_stream_state_changed;
  PinosListener on_stream_format_changed;

  SpaVideoInfoRaw format;
  int32_t stride;

  uint8_t params_buffer[1024];
  int counter;
  uint32_t seq;
} Data;

static void
on_timeout (SpaLoopUtils *utils,
            SpaSource    *source,
            void         *userdata)
{
  Data *data = userdata;
  uint32_t id;
  SpaBuffer *buf;
  int i, j;
  uint8_t *p, *map;
  SpaMetaHeader *h;

  id = pinos_stream_get_empty_buffer (data->stream);
  if (id == SPA_ID_INVALID)
    return;

  buf = pinos_stream_peek_buffer (data->stream, id);

  if (buf->datas[0].type == data->type.data.MemFd) {
    map = mmap (NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset, PROT_READ | PROT_WRITE,
                    MAP_SHARED, buf->datas[0].fd, 0);
    if (map == MAP_FAILED) {
      printf ("failed to mmap: %s\n", strerror (errno));
      return;
    }
    p = SPA_MEMBER (map, buf->datas[0].mapoffset, uint8_t);
  }
  else if (buf->datas[0].type == data->type.data.MemPtr) {
    map = NULL;
    p = buf->datas[0].data;
  } else
    return;

  if ((h =  spa_buffer_find_meta (buf, data->type.meta.Header))) {
    struct timespec now;
    h->flags = 0;
    h->seq = data->seq++;
    clock_gettime (CLOCK_MONOTONIC, &now);
    h->pts = SPA_TIMESPEC_TO_TIME (&now);
    h->dts_offset = 0;
  }

  for (i = 0; i < data->format.size.height; i++) {
    for (j = 0; j < data->format.size.width * BPP; j++) {
      p[j] = data->counter + j * i;
    }
    p += buf->datas[0].chunk->stride;
    data->counter += 13;
  }

  if (map)
    munmap (map, buf->datas[0].maxsize + buf->datas[0].mapoffset);

  pinos_stream_send_buffer (data->stream, id);
}

static void
on_stream_state_changed (PinosListener  *listener,
                         PinosStream    *stream)
{
  Data *data = SPA_CONTAINER_OF (listener, Data, on_stream_state_changed);

  printf ("stream state: \"%s\"\n", pinos_stream_state_as_string (stream->state));

  switch (stream->state) {
    case PINOS_STREAM_STATE_PAUSED:
      pinos_loop_update_timer (data->loop,
                               data->timer,
                               NULL,
                               NULL,
                               false);
      break;

    case PINOS_STREAM_STATE_STREAMING:
    {
      struct timespec timeout, interval;

      timeout.tv_sec = 0;
      timeout.tv_nsec = 1;
      interval.tv_sec = 0;
      interval.tv_nsec = 40 * SPA_NSEC_PER_MSEC;

      pinos_loop_update_timer (data->loop,
                               data->timer,
                               &timeout,
                               &interval,
                               false);
      break;
    }
    default:
      break;
  }
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)

static void
on_stream_format_changed (PinosListener  *listener,
                          PinosStream    *stream,
                          SpaFormat      *format)
{
  Data *data = SPA_CONTAINER_OF (listener, Data, on_stream_format_changed);
  PinosContext *ctx = stream->context;
  SpaPODBuilder b = { NULL };
  SpaPODFrame f[2];
  SpaParam *params[2];

  if (format) {
    spa_format_video_raw_parse (format, &data->format, &data->type.format_video);

    data->stride = SPA_ROUND_UP_N (data->format.size.width * BPP, 4);

    spa_pod_builder_init (&b, data->params_buffer, sizeof (data->params_buffer));
    spa_pod_builder_object (&b, &f[0], 0, ctx->type.param_alloc_buffers.Buffers,
        PROP      (&f[1], ctx->type.param_alloc_buffers.size,    SPA_POD_TYPE_INT,
                                                               data->stride * data->format.size.height),
        PROP      (&f[1], ctx->type.param_alloc_buffers.stride,  SPA_POD_TYPE_INT, data->stride),
        PROP_U_MM (&f[1], ctx->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT, 32, 2, 32),
        PROP      (&f[1], ctx->type.param_alloc_buffers.align,   SPA_POD_TYPE_INT, 16));
    params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaParam);

    spa_pod_builder_object (&b, &f[0], 0, ctx->type.param_alloc_meta_enable.MetaEnable,
      PROP      (&f[1], ctx->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID, ctx->type.meta.Header),
      PROP      (&f[1], ctx->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaParam);

    pinos_stream_finish_format (stream, SPA_RESULT_OK, params, 2);
  }
  else {
    pinos_stream_finish_format (stream, SPA_RESULT_OK, NULL, 0);
  }
}

static void
on_state_changed (PinosListener  *listener,
                  PinosContext   *context)
{
  Data *data = SPA_CONTAINER_OF (listener, Data, on_state_changed);

  switch (context->state) {
    case PINOS_CONTEXT_STATE_ERROR:
      printf ("context error: %s\n", context->error);
      data->running = false;
      break;

    case PINOS_CONTEXT_STATE_CONNECTED:
    {
      SpaFormat *formats[1];
      uint8_t buffer[1024];
      SpaPODBuilder b = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
      SpaPODFrame f[2];

      printf ("context state: \"%s\"\n", pinos_context_state_as_string (context->state));

      data->stream = pinos_stream_new (context, "video-src", NULL);

      spa_pod_builder_format (&b, &f[0], data->type.format,
         data->type.media_type.video, data->type.media_subtype.raw,
         PROP      (&f[1], data->type.format_video.format,    SPA_POD_TYPE_ID,  data->type.video_format.RGB),
         PROP_U_MM (&f[1], data->type.format_video.size,      SPA_POD_TYPE_RECTANGLE, 320, 240,
                                                                                      1, 1,
                                                                                      4096, 4096),
         PROP      (&f[1], data->type.format_video.framerate, SPA_POD_TYPE_FRACTION,  25, 1));
      formats[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

      pinos_signal_add (&data->stream->state_changed,
                        &data->on_stream_state_changed,
                        on_stream_state_changed);
      pinos_signal_add (&data->stream->format_changed,
                        &data->on_stream_format_changed,
                        on_stream_format_changed);

      pinos_stream_connect (data->stream,
                            PINOS_DIRECTION_OUTPUT,
                            PINOS_STREAM_MODE_BUFFER,
                            NULL,
                            PINOS_STREAM_FLAG_NONE,
                            1,
                            formats);
      break;
    }
    default:
      printf ("context state: \"%s\"\n", pinos_context_state_as_string (context->state));
      break;
  }
}

int
main (int argc, char *argv[])
{
  Data data = { 0, };

  pinos_init (&argc, &argv);

  data.loop = pinos_loop_new ();
  data.running = true;
  data.context = pinos_context_new (data.loop, "video-src", NULL);

  init_type (&data.type, data.context->type.map);

  data.timer = pinos_loop_add_timer (data.loop, on_timeout, &data);

  pinos_signal_add (&data.context->state_changed,
                    &data.on_state_changed,
                    on_state_changed);

  pinos_context_connect (data.context,
                         PINOS_CONTEXT_FLAG_NO_REGISTRY);

  pinos_loop_enter (data.loop);
  while (data.running) {
    pinos_loop_iterate (data.loop, -1);
  }
  pinos_loop_leave (data.loop);

  pinos_context_destroy (data.context);
  pinos_loop_destroy (data.loop);

  return 0;
}
