/* Pinos
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
#include <errno.h>
#include <poll.h>

#include <spa/log.h>
#include <spa/id-map.h>
#include <spa/monitor.h>
#include <spa/poll.h>
#include <spa/debug.h>

typedef struct {
  uint32_t monitor;
} URI;

typedef struct {
  URI uri;

  SpaIDMap *map;
  SpaLog *log;
  SpaPoll main_loop;

  SpaSupport support[3];
  unsigned int n_support;

  unsigned int n_poll;
  SpaPollItem poll[16];

  bool rebuild_fds;
  SpaPollFd fds[16];
  unsigned int n_fds;
} AppData;


static void
inspect_item (SpaMonitorItem *item)
{
  fprintf (stderr, "  name:  %s\n", item->name);
  fprintf (stderr, "  class: %s\n", item->klass);
  if (item->info)
    spa_debug_dict (item->info);
}

static void
on_monitor_event  (SpaMonitor      *monitor,
                   SpaMonitorEvent *event,
                   void            *user_data)
{
  switch (event->type) {
    case SPA_MONITOR_EVENT_TYPE_ADDED:
    {
      SpaMonitorItem *item = event->data;
      fprintf (stderr, "added:\n");
      inspect_item (item);
      break;
    }
    case SPA_MONITOR_EVENT_TYPE_REMOVED:
    {
      SpaMonitorItem *item = event->data;
      fprintf (stderr, "removed:\n");
      inspect_item (item);
      break;
    }
    case SPA_MONITOR_EVENT_TYPE_CHANGED:
    {
      SpaMonitorItem *item = event->data;
      fprintf (stderr, "changed:\n");
      inspect_item (item);
      break;
    }
    default:
      break;
  }
}

static SpaResult
do_add_item (SpaPoll     *poll,
             SpaPollItem *item)
{
  AppData *data = SPA_CONTAINER_OF (poll, AppData, main_loop);

  data->poll[data->n_poll] = *item;
  data->n_poll++;
  if (item->n_fds)
    data->rebuild_fds = true;

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
static void
handle_monitor (AppData *data, SpaMonitor *monitor)
{
  SpaResult res;
  void *state = NULL;

  if (monitor->info)
    spa_debug_dict (monitor->info);

  while (true) {
    SpaMonitorItem *item;

    if ((res = spa_monitor_enum_items (monitor, &item, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("spa_monitor_enum_items: got error %d\n", res);
      break;
    }
    inspect_item (item);
  }

  spa_monitor_set_event_callback (monitor, on_monitor_event, &data);

  while (true) {
    SpaPollNotifyData ndata;
    int i, j, r;

    /* rebuild */
    if (data->rebuild_fds) {
      data->n_fds = 0;
      for (i = 0; i < data->n_poll; i++) {
        SpaPollItem *p = &data->poll[i];

        if (!p->enabled)
          continue;

        for (j = 0; j < p->n_fds; j++)
          data->fds[data->n_fds + j] = p->fds[j];
        p->fds = &data->fds[data->n_fds];
        data->n_fds += p->n_fds;
      }
      data->rebuild_fds = false;
    }

    r = poll ((struct pollfd *) data->fds, data->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      fprintf (stderr, "monitor %p: select timeout", monitor);
      break;
    }

    /* after */
    for (i = 0; i < data->n_poll; i++) {
      SpaPollItem *p = &data->poll[i];

      if (p->enabled && p->after_cb) {
        ndata.fds = p->fds;
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        p->after_cb (&ndata);
      }
    }
  }
}

int
main (int argc, char *argv[])
{
  AppData data = { 0 };
  SpaResult res;
  void *handle;
  SpaEnumHandleFactoryFunc enum_func;
  void *fstate = NULL;

  data.map = spa_id_map_get_default ();
  data.log = NULL;
  data.main_loop.handle = NULL;
  data.main_loop.size = sizeof (SpaPoll);
  data.main_loop.info = NULL;
  data.main_loop.add_item = do_add_item;
  data.main_loop.update_item = do_update_item;
  data.main_loop.remove_item = do_remove_item;

  data.support[0].uri = SPA_ID_MAP_URI;
  data.support[0].data = data.map;
  data.support[1].uri = SPA_LOG_URI;
  data.support[1].data = data.log;
  data.support[2].uri = SPA_POLL__MainLoop;
  data.support[2].data = &data.main_loop;
  data.n_support = 3;

  data.uri.monitor = spa_id_map_get_id (data.map, SPA_MONITOR_URI);

  if (argc < 2) {
    printf ("usage: %s <plugin.so>\n", argv[0]);
    return -1;
  }

  if ((handle = dlopen (argv[1], RTLD_NOW)) == NULL) {
    printf ("can't load %s\n", argv[1]);
    return -1;
  }
  if ((enum_func = dlsym (handle, "spa_enum_handle_factory")) == NULL) {
    printf ("can't find function\n");
    return -1;
  }

  while (true) {
    const SpaHandleFactory *factory;
    void *istate = NULL;

    if ((res = enum_func (&factory, &fstate)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("can't enumerate factories: %d\n", res);
      break;
    }

    while (true) {
      const SpaInterfaceInfo *info;

      if ((res = spa_handle_factory_enum_interface_info (factory, &info, &istate)) < 0) {
        if (res != SPA_RESULT_ENUM_END)
          printf ("can't enumerate interfaces: %d\n", res);
        break;
      }

      if (!strcmp (info->uri, SPA_MONITOR_URI)) {
        SpaHandle *handle;
        void *interface;

        handle = calloc (1, factory->size);
        if ((res = spa_handle_factory_init (factory, handle, NULL, data.support, data.n_support)) < 0) {
          printf ("can't make factory instance: %d\n", res);
          continue;
        }

        if ((res = spa_handle_get_interface (handle, data.uri.monitor, &interface)) < 0) {
          printf ("can't get interface: %d\n", res);
          continue;
        }
        handle_monitor (&data, interface);
      }
    }
  }

  return 0;
}
