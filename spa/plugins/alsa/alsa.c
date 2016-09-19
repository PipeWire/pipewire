/* Spa ALSA support
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

#include <spa/plugin.h>
#include <spa/node.h>

extern const SpaHandleFactory spa_alsa_source_factory;
extern const SpaHandleFactory spa_alsa_sink_factory;
extern const SpaHandleFactory spa_alsa_monitor_factory;

SpaResult
spa_enum_handle_factory (const SpaHandleFactory **factory,
                         void                   **state)
{
  int index;

  if (factory == NULL || state == NULL)
    return SPA_RESULT_ENUM_END;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *factory = &spa_alsa_source_factory;
      break;
    case 1:
      *factory = &spa_alsa_sink_factory;
      break;
    case 2:
      *factory = &spa_alsa_monitor_factory;
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}
