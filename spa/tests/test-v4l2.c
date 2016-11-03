/* Spa
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>

#include <SDL2/SDL.h>

#include <spa/id-map.h>
#include <spa/log.h>
#include <spa/node.h>
#include <spa/video/format.h>
#include <lib/debug.h>
#include <lib/props.h>
#include <lib/mapper.h>

#define MAX_BUFFERS     8

typedef struct {
  SpaBuffer buffer;
  SpaMeta metas[2];
  SpaMetaHeader header;
  SpaMetaPointer ptr;
  SpaData datas[1];
} SDLBuffer;

typedef struct {
  uint32_t node;
} URI;

typedef struct {
  SpaNode *source;

  SDL_Renderer *renderer;
  SDL_Window *window;
  SDL_Texture *texture;

  bool use_buffer;

  bool running;
  pthread_t thread;
  SpaPollFd fds[16];
  unsigned int n_fds;
  SpaPollItem poll;

  SpaBuffer *bp[MAX_BUFFERS];
  SDLBuffer buffers[MAX_BUFFERS];
  unsigned int n_buffers;

  SpaSupport support[2];
  unsigned int n_support;
  SpaIDMap *map;
  SpaLog *log;
  SpaPoll data_loop;
  URI uri;
} AppData;

static SpaResult
make_node (AppData *data, SpaNode **node, const char *lib, const char *name)
{
  SpaHandle *handle;
  SpaResult res;
  void *hnd;
  SpaEnumHandleFactoryFunc enum_func;
  unsigned int i;
  void *state = NULL;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    printf ("can't load %s: %s\n", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    printf ("can't find enum function\n");
    return SPA_RESULT_ERROR;
  }

  for (i = 0; ;i++) {
    const SpaHandleFactory *factory;
    void *iface;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("can't enumerate factories: %d\n", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    handle = calloc (1, factory->size);
    if ((res = spa_handle_factory_init (factory, handle, NULL, data->support, data->n_support)) < 0) {
      printf ("can't make factory instance: %d\n", res);
      return res;
    }
    if ((res = spa_handle_get_interface (handle, data->uri.node, &iface)) < 0) {
      printf ("can't get interface %d\n", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void
on_source_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    {
      SpaPortOutputInfo info[1] = { 0, };
      SpaResult res;
      SpaBuffer *b;
      void *sdata, *ddata;
      int sstride, dstride;
      int i;
      uint8_t *src, *dst;
      SpaMeta *metas;
      SpaData *datas;

      if ((res = spa_node_port_pull_output (data->source, 1, info)) < 0)
        printf ("got pull error %d\n", res);

      b = data->bp[info->buffer_id];
      metas = b->metas;
      datas = b->datas;

      if (metas[1].type == SPA_META_TYPE_POINTER &&
          strcmp (((SpaMetaPointer *)metas[1].data)->ptr_type, "SDL_Texture") == 0) {
        SDL_Texture *texture;
        texture = ((SpaMetaPointer *)metas[1].data)->ptr;

        SDL_UnlockTexture(texture);

        SDL_RenderClear (data->renderer);
        SDL_RenderCopy (data->renderer, texture, NULL, NULL);
        SDL_RenderPresent (data->renderer);

        if (SDL_LockTexture (texture, NULL, &sdata, &sstride) < 0) {
          fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
          return;
        }
        datas[0].type = SPA_DATA_TYPE_MEMPTR;
        datas[0].data = sdata;
        datas[0].offset = 0;
        datas[0].size = sstride * 240;
        datas[0].maxsize = sstride * 240;
        datas[0].stride = sstride;
      } else {
        if (SDL_LockTexture (data->texture, NULL, &ddata, &dstride) < 0) {
          fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
          return;
        }
        sdata = datas[0].data;
        sstride = datas[0].stride;

        for (i = 0; i < 240; i++) {
          src = ((uint8_t*)sdata + i * sstride);
          dst = ((uint8_t*)ddata + i * dstride);
          memcpy (dst, src, SPA_MIN (sstride, dstride));
        }
        SDL_UnlockTexture(data->texture);

        SDL_RenderClear (data->renderer);
        SDL_RenderCopy (data->renderer, data->texture, NULL, NULL);
        SDL_RenderPresent (data->renderer);
      }
      spa_node_port_reuse_buffer (data->source, 0, info->buffer_id);
      break;
    }
    default:
      printf ("got event %d\n", event->type);
      break;
  }
}

static SpaResult
do_add_item (SpaPoll     *poll,
             SpaPollItem *item)
{
  AppData *data = SPA_CONTAINER_OF (poll, AppData, data_loop);
  int i;

  data->poll = *item;
  for (i = 0; i < data->poll.n_fds; i++) {
    data->fds[i] = item->fds[i];
  }
  data->n_fds = data->poll.n_fds;
  data->poll.fds = data->fds;

  return SPA_RESULT_OK;
}

static SpaResult
do_update_item (SpaPoll     *poll,
             SpaPollItem *item)
{
  return SPA_RESULT_OK;
}

static SpaResult
do_remove_item (SpaPoll     *poll,
             SpaPollItem *item)
{
  return SPA_RESULT_OK;
}

static SpaResult
make_nodes (AppData *data, const char *device)
{
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = make_node (data, &data->source, "spa/plugins/v4l2/libspa-v4l2.so", "v4l2-source")) < 0) {
    printf ("can't create v4l2-source: %d\n", res);
    return res;
  }
  spa_node_set_event_callback (data->source, on_source_event, data);

  if ((res = spa_node_get_props (data->source, &props)) < 0)
    printf ("got get_props error %d\n", res);

  value.value = device ? device : "/dev/video7";
  value.size = strlen (value.value)+1;
  spa_props_set_value (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = spa_node_set_props (data->source, props)) < 0)
    printf ("got set_props error %d\n", res);

  return res;
}

static void
alloc_buffers (AppData *data)
{
  int i;

  for (i = 0; i < MAX_BUFFERS; i++) {
    SDLBuffer *b = &data->buffers[i];
    SDL_Texture *texture;
    void *ptr;
    int stride;

    data->bp[i] = &b->buffer;

    texture = SDL_CreateTexture (data->renderer,
                                 SDL_PIXELFORMAT_YUY2,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 320, 240);
    if (!texture) {
      printf ("can't create texture: %s\n", SDL_GetError ());
      return;
    }
    if (SDL_LockTexture (texture, NULL, &ptr, &stride) < 0) {
      fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
      return;
    }

    b->buffer.id = i;
    b->buffer.n_metas = 2;
    b->buffer.metas = b->metas;
    b->buffer.n_datas = 1;
    b->buffer.datas = b->datas;

    b->header.flags = 0;
    b->header.seq = 0;
    b->header.pts = 0;
    b->header.dts_offset = 0;
    b->metas[0].type = SPA_META_TYPE_HEADER;
    b->metas[0].data = &b->header;
    b->metas[0].size = sizeof (b->header);

    b->ptr.ptr_type = "SDL_Texture";
    b->ptr.ptr = texture;
    b->metas[1].type = SPA_META_TYPE_POINTER;
    b->metas[1].data = &b->ptr;
    b->metas[1].size = sizeof (b->ptr);

    b->datas[0].type = SPA_DATA_TYPE_MEMPTR;
    b->datas[0].data = ptr;
    b->datas[0].offset = 0;
    b->datas[0].size = stride * 240;
    b->datas[0].maxsize = stride * 240;
    b->datas[0].stride = stride;
  }
  data->n_buffers = MAX_BUFFERS;

  spa_node_port_use_buffers (data->source, SPA_DIRECTION_OUTPUT, 0, data->bp, MAX_BUFFERS);
}

typedef struct {
  SpaFormat fmt;
  SpaPropInfo infos[3];
  SpaVideoFormat format;
  SpaRectangle size;
  SpaFraction framerate;
} VideoFormat;

static SpaResult
negotiate_formats (AppData *data)
{
  SpaResult res;
  const SpaPortInfo *info;
  VideoFormat f;

#if 0
  void *state = NULL;

  if ((res = spa_node_port_enum_formats (data->source, 0, &format, NULL, &state)) < 0)
    return res;
#else
  f.fmt.media_type = SPA_MEDIA_TYPE_VIDEO;
  f.fmt.media_subtype = SPA_MEDIA_SUBTYPE_RAW;
  f.fmt.props.n_prop_info = 3;
  f.fmt.props.prop_info = f.infos;

  spa_prop_info_fill_video (&f.infos[0],
                            SPA_PROP_ID_VIDEO_FORMAT,
                            offsetof (VideoFormat, format));
  f.format = SPA_VIDEO_FORMAT_YUY2;

  spa_prop_info_fill_video (&f.infos[1],
                            SPA_PROP_ID_VIDEO_SIZE,
                            offsetof (VideoFormat, size));
  f.size.width = 320;
  f.size.height = 240;

  spa_prop_info_fill_video (&f.infos[2],
                            SPA_PROP_ID_VIDEO_FRAMERATE,
                            offsetof (VideoFormat, framerate));
  f.framerate.num = 25;
  f.framerate.denom = 1;
#endif

  if ((res = spa_node_port_set_format (data->source, SPA_DIRECTION_OUTPUT, 0, false, &f.fmt)) < 0)
    return res;

  if ((res = spa_node_port_get_info (data->source, SPA_DIRECTION_OUTPUT, 0, &info)) < 0)
    return res;

  spa_debug_port_info (info);

  if (data->use_buffer) {
    alloc_buffers (data);
  } else {
    unsigned int n_buffers;

    data->texture = SDL_CreateTexture (data->renderer,
                                       SDL_PIXELFORMAT_YUY2,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       320, 240);
    if (!data->texture) {
      printf ("can't create texture: %s\n", SDL_GetError ());
      return -1;
    }
    n_buffers = MAX_BUFFERS;
    if ((res = spa_node_port_alloc_buffers (data->source, SPA_DIRECTION_OUTPUT, 0, NULL, 0, data->bp, &n_buffers)) < 0) {
      printf ("can't allocate buffers: %s\n", SDL_GetError ());
      return -1;
    }
    data->n_buffers = n_buffers;
  }
  return SPA_RESULT_OK;
}

static void *
loop (void *user_data)
{
  AppData *data = user_data;
  int r;

  printf ("enter thread\n");
  while (data->running) {
    SpaPollNotifyData ndata;

    r = poll ((struct pollfd *) data->fds, data->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      fprintf (stderr, "select timeout\n");
      break;
    }
    if (data->poll.after_cb) {
      ndata.fds = data->poll.fds;
      ndata.n_fds = data->poll.n_fds;
      ndata.user_data = data->poll.user_data;
      data->poll.after_cb (&ndata);
    }
  }
  printf ("leave thread\n");
  return NULL;
}

static void
run_async_source (AppData *data)
{
  SpaResult res;
  SpaNodeCommand cmd;
  int err;

  cmd.type = SPA_NODE_COMMAND_START;
  if ((res = spa_node_send_command (data->source, &cmd)) < 0)
    printf ("got error %d\n", res);

  data->running = true;
  if ((err = pthread_create (&data->thread, NULL, loop, data)) != 0) {
    printf ("can't create thread: %d %s", err, strerror (err));
    data->running = false;
  }

  sleep (10);

  if (data->running) {
    data->running = false;
    pthread_join (data->thread, NULL);
  }

  cmd.type = SPA_NODE_COMMAND_PAUSE;
  if ((res = spa_node_send_command (data->source, &cmd)) < 0)
    printf ("got error %d\n", res);
}

int
main (int argc, char *argv[])
{
  AppData data;
  SpaResult res;

  data.use_buffer = true;

  data.map = spa_id_map_get_default ();

  data.data_loop.size = sizeof (SpaPoll);
  data.data_loop.info = NULL;
  data.data_loop.add_item = do_add_item;
  data.data_loop.update_item = do_update_item;
  data.data_loop.remove_item = do_remove_item;

  data.support[0].uri = SPA_ID_MAP_URI;
  data.support[0].data = data.map;
  data.support[1].uri = SPA_POLL__DataLoop;
  data.support[1].data = &data.data_loop;
  data.n_support = 2;

  data.uri.node = spa_id_map_get_id (data.map, SPA_NODE_URI);

  if (SDL_Init (SDL_INIT_VIDEO) < 0) {
    printf ("can't initialize SDL: %s\n", SDL_GetError ());
    return -1;
  }

  if (SDL_CreateWindowAndRenderer (320, 240, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
    printf ("can't create window: %s\n", SDL_GetError ());
    return -1;
  }


  if ((res = make_nodes (&data, argv[1])) < 0) {
    printf ("can't make nodes: %d\n", res);
    return -1;
  }
  if ((res = negotiate_formats (&data)) < 0) {
    printf ("can't negotiate nodes: %d\n", res);
    return -1;
  }

  run_async_source (&data);

  SDL_DestroyRenderer (data.renderer);

  return 0;
}
