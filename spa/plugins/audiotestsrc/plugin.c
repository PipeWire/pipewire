/* Spa Volume plugin
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

extern const SpaHandleFactory spa_audiotestsrc_factory;

SpaResult
spa_enum_handle_factory (const SpaHandleFactory **factory,
                         uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  switch (index) {
    case 0:
      *factory = &spa_audiotestsrc_factory;
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}
