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
#include <sys/mman.h>

#include <spa/include/spa/type-map.h>
#include <spa/include/spa/format-utils.h>
#include <spa/include/spa/video/format-utils.h>
#include <spa/include/spa/format-builder.h>
#include <spa/include/spa/props.h>

#include <pinos/client/pinos.h>
#include <pinos/client/sig.h>
#include <spa/lib/debug.h>

typedef struct {
  uint32_t format;
  uint32_t props;
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
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
}

#define WIDTH 320
#define HEIGHT 240
#define BPP    3
#define STRIDE (BPP * WIDTH)

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

  uint8_t params_buffer[1024];
  int counter;
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

  id = pinos_stream_get_empty_buffer (data->stream);
  if (id == SPA_ID_INVALID)
    return;

  buf = pinos_stream_peek_buffer (data->stream, id);

  if (buf->datas[0].type == SPA_DATA_TYPE_MEMFD) {
    map = mmap (NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset, PROT_READ | PROT_WRITE,
                    MAP_SHARED, buf->datas[0].fd, 0);
    p = SPA_MEMBER (map, buf->datas[0].mapoffset, uint8_t);
  }
  else if (buf->datas[0].type == SPA_DATA_TYPE_MEMPTR) {
    map = NULL;
    p = buf->datas[0].data;
  } else
    return;

  for (i = 0; i < HEIGHT; i++) {
    for (j = 0; j < WIDTH * BPP; j++) {
      p[j] = data->counter + j * i;
    }
    p += STRIDE;
    data->counter += 13;
  }

  if (map)
    munmap (map, buf->datas[0].maxsize);

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
  SpaAllocParam *params[2];

  if (format) {
    spa_pod_builder_init (&b, data->params_buffer, sizeof (data->params_buffer));
    spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_buffers.Buffers,
        PROP      (&f[1], ctx->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT, STRIDE * HEIGHT),
        PROP      (&f[1], ctx->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, STRIDE),
        PROP_U_MM (&f[1], ctx->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, 32, 2, 32),
        PROP      (&f[1], ctx->type.alloc_param_buffers.align,   SPA_POD_TYPE_INT, 16));
    params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_meta_enable.MetaEnable,
      PROP      (&f[1], ctx->type.alloc_param_meta_enable.type, SPA_POD_TYPE_INT, SPA_META_TYPE_HEADER));
    params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

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
         PROP (&f[1], data->type.format_video.format,    SPA_POD_TYPE_ID,  data->type.video_format.RGB),
         PROP (&f[1], data->type.format_video.size,      SPA_POD_TYPE_RECTANGLE, WIDTH, HEIGHT),
         PROP (&f[1], data->type.format_video.framerate, SPA_POD_TYPE_FRACTION,  25, 1));

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

  pinos_context_connect (data.context);

  pinos_loop_enter (data.loop);
  while (data.running) {
    pinos_loop_iterate (data.loop, -1);
  }
  pinos_loop_leave (data.loop);

  pinos_context_destroy (data.context);
  pinos_loop_destroy (data.loop);

  return 0;
}
