/* Simple Plugin API
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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/id-map.h>
#include <spa/log.h>
#include <spa/buffer.h>
#include <spa/clock.h>
#include <spa/format.h>
#include <spa/monitor.h>
#include <spa/node.h>
#include <spa/node-command.h>
#include <spa/node-event.h>
#include <spa/poll.h>
#include <spa/port.h>
#include <spa/props.h>
#include <spa/queue.h>
#include <spa/ringbuffer.h>
#include <spa/debug.h>

static const char *uris[] = {
  NULL,
  SPA_ID_MAP_URI,
  SPA_LOG_URI,
  SPA_BUFFER_URI,
  SPA_CLOCK_URI,
  SPA_MONITOR_URI,
  SPA_NODE_URI,
  SPA_NODE_COMMAND_URI,
  SPA_NODE_EVENT_URI,
  SPA_ALLOC_PARAM_URI,
  SPA_PROPS_URI,
  SPA_QUEUE_URI,
  SPA_RINGBUFFER_URI,
  SPA_POLL__MainLoop,
  SPA_POLL__DataLoop,
};

static uint32_t
id_map_get_id (SpaIDMap *map, const char *uri)
{
  if (uri != NULL) {
    unsigned int i;
    for (i = 1; i < SPA_N_ELEMENTS (uris); i++) {
      if (strcmp (uris[i], uri) == 0)
        return i;
    }
  }
  return 0;
}

static const char *
id_map_get_uri (SpaIDMap *map, uint32_t id)
{
  if (id < SPA_N_ELEMENTS (uris))
    return uris[id];
  return 0;
}

static const SpaIDMap default_map = {
  NULL,
  sizeof (SpaIDMap),
  NULL,
  id_map_get_id,
  id_map_get_uri,
};

SpaIDMap *
spa_id_map_get_default (void)
{
  return (SpaIDMap *) &default_map;
}
