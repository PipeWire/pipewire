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

#include <spa/node.h>
#include <spa/debug.h>
#include <spa/video/format.h>

#define USE_BUFFER

#define MAX_BUFFERS     8

typedef struct {
  SpaBuffer buffer;
  SpaMeta metas[2];
  SpaMetaHeader header;
  SpaMetaPointer ptr;
  SpaData datas[1];
} SDLBuffer;

typedef struct {
  SpaHandle *source;
  const SpaNode *source_node;
  SDL_Renderer *renderer;
  SDL_Window *window;
  SDL_Texture *texture;
  bool running;
  pthread_t thread;
  SpaPollFd fds[16];
  unsigned int n_fds;
  SpaPollItem poll;
  SpaBuffer *bp[MAX_BUFFERS];
  SDLBuffer buffers[MAX_BUFFERS];
} AppData;

static SpaResult
make_node (SpaHandle **handle, const SpaNode **node, const char *lib, const char *name)
{
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
    const void *iface;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("can't enumerate factories: %d\n", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    *handle = calloc (1, factory->size);
    if ((res = factory->init (factory, *handle)) < 0) {
      printf ("can't make factory instance: %d\n", res);
      return res;
    }
    if ((res = (*handle)->get_interface (*handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      printf ("can't get interface %d\n", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}
#define MIN(a,b) (a < b ? a : b)

static void
on_source_event (SpaHandle *handle, SpaEvent *event, void *user_data)
{
  AppData *data = user_data;

  switch (event->type) {
    case SPA_EVENT_TYPE_CAN_PULL_OUTPUT:
    {
      SpaOutputInfo info[1] = { 0, };
      SpaResult res;
      SpaBuffer *b;
      void *sdata, *ddata;
      int sstride, dstride;
      int i;
      uint8_t *src, *dst;

      if ((res = data->source_node->port_pull_output (data->source, 1, info)) < 0)
        printf ("got pull error %d\n", res);

      b = info[0].buffer;

      if (b->metas[1].type == SPA_META_TYPE_POINTER &&
          strcmp (((SpaMetaPointer*)b->metas[1].data)->ptr_type, "SDL_Texture") == 0) {
        SDL_Texture *texture;
        texture = ((SpaMetaPointer*)b->metas[1].data)->ptr;

        SDL_UnlockTexture(texture);

        SDL_RenderClear (data->renderer);
        SDL_RenderCopy (data->renderer, texture, NULL, NULL);
        SDL_RenderPresent (data->renderer);

        if (SDL_LockTexture (texture, NULL, &sdata, &sstride) < 0) {
          fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
          return;
        }
        b->datas[0].ptr = sdata;
        b->datas[0].ptr_type = "sysmem";
        b->datas[0].size = sstride * 240;
        b->datas[0].stride = sstride;
      } else {
        if (SDL_LockTexture (data->texture, NULL, &ddata, &dstride) < 0) {
          fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
          return;
        }
        sdata = b->datas[0].ptr;
        sstride = b->datas[0].stride;

        for (i = 0; i < 240; i++) {
          src = ((uint8_t*)sdata + i * sstride);
          dst = ((uint8_t*)ddata + i * dstride);
          memcpy (dst, src, MIN (sstride, dstride));
        }
        SDL_UnlockTexture(data->texture);

        SDL_RenderClear (data->renderer);
        SDL_RenderCopy (data->renderer, data->texture, NULL, NULL);
        SDL_RenderPresent (data->renderer);
      }
      spa_buffer_unref (b);
      break;
    }
    case SPA_EVENT_TYPE_ADD_POLL:
    {
      SpaPollItem *poll = event->data;

      data->poll = *poll;
      data->fds[0] = poll->fds[0];
      data->n_fds = 1;
      data->poll.fds = data->fds;
      break;
    }
    default:
      printf ("got event %d\n", event->type);
      break;
  }
}

static SpaResult
make_nodes (AppData *data, const char *device)
{
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = make_node (&data->source, &data->source_node, "plugins/v4l2/libspa-v4l2.so", "v4l2-source")) < 0) {
    printf ("can't create v4l2-source: %d\n", res);
    return res;
  }
  data->source_node->set_event_callback (data->source, on_source_event, data);

  if ((res = data->source_node->get_props (data->source, &props)) < 0)
    printf ("got get_props error %d\n", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.value = device ? device : "/dev/video0";
  value.size = strlen (value.value)+1;
  props->set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = data->source_node->set_props (data->source, props)) < 0)
    printf ("got set_props error %d\n", res);
  return res;
}

#ifdef USE_BUFFER
static void
alloc_buffers (AppData *data)
{
  int i;

  for (i = 0; i < MAX_BUFFERS; i++) {
    SDLBuffer *b = &data->buffers[i];
    SDL_Texture *texture;
    void *mem;
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
    if (SDL_LockTexture (texture, NULL, &mem, &stride) < 0) {
      fprintf (stderr, "Couldn't lock texture: %s\n", SDL_GetError());
      return;
    }

    b->buffer.refcount = 1;
    b->buffer.notify = NULL;
    b->buffer.size = stride * 240;
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
    b->datas[0].ptr = mem;
    b->datas[0].ptr_type = "sysmem";
    b->datas[0].offset = 0;
    b->datas[0].size = stride * 240;
    b->datas[0].stride = stride;
  }
  data->source_node->port_use_buffers (data->source, 0, data->bp, MAX_BUFFERS);
}
#endif

static SpaResult
negotiate_formats (AppData *data)
{
  SpaResult res;
  SpaFormat *format;
  SpaProps *props;
  uint32_t val;
  SpaFraction frac;
  SpaPropValue value;
  const SpaPortInfo *info;
  SpaRectangle size;
  void *state = NULL;

  if ((res = data->source_node->port_enum_formats (data->source, 0, &format, NULL, &state)) < 0)
    return res;

  props = &format->props;

  value.type = SPA_PROP_TYPE_UINT32;
  value.size = sizeof (uint32_t);
  value.value = &val;

  val = SPA_VIDEO_FORMAT_YUY2;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_FORMAT), &value)) < 0)
    return res;

  value.type = SPA_PROP_TYPE_RECTANGLE;
  value.size = sizeof (SpaRectangle);
  value.value = &size;
  size.width = 320;
  size.height = 240;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_SIZE), &value)) < 0)
    return res;

  value.type = SPA_PROP_TYPE_FRACTION;
  value.size = sizeof (SpaFraction);
  value.value = &frac;
  frac.num = 25;
  frac.denom = 1;

  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_FRAMERATE), &value)) < 0)
    return res;

  if ((res = data->source_node->port_set_format (data->source, 0, false, format)) < 0)
    return res;

  if ((res = data->source_node->port_get_info (data->source, 0, &info)) < 0)
    return res;

  spa_debug_port_info (info);

#ifdef USE_BUFFER
  alloc_buffers (data);
#else
  data->texture = SDL_CreateTexture (data->renderer,
                                     SDL_PIXELFORMAT_YUY2,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     320, 240);
  if (!data->texture) {
    printf ("can't create texture: %s\n", SDL_GetError ());
    return -1;
  }
#endif

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
  SpaCommand cmd;
  bool done = false;
  int err;

  cmd.type = SPA_COMMAND_START;
  if ((res = data->source_node->send_command (data->source, &cmd)) < 0)
    printf ("got error %d\n", res);

  data->running = true;
  if ((err = pthread_create (&data->thread, NULL, loop, data)) != 0) {
    printf ("can't create thread: %d %s", err, strerror (err));
    data->running = false;
  }

  while (!done) {
    SDL_Event event;

    while (SDL_PollEvent (&event)) {
      switch (event.type) {
        case SDL_KEYDOWN:
          if (event.key.keysym.sym == SDLK_ESCAPE) {
            done = SDL_TRUE;
          }
          break;
        case SDL_QUIT:
          done = SDL_TRUE;
          break;
      }
      sleep (1);
    }
  }

  if (data->running) {
    data->running = false;
    pthread_join (data->thread, NULL);
  }

  cmd.type = SPA_COMMAND_STOP;
  if ((res = data->source_node->send_command (data->source, &cmd)) < 0)
    printf ("got error %d\n", res);
}

int
main (int argc, char *argv[])
{
  AppData data;
  SpaResult res;

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
