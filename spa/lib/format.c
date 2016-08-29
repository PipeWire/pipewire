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

#include <spa/format.h>

SpaResult
spa_format_to_string (const SpaFormat *format, char **result)
{
  if (format == NULL || result == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  return SPA_RESULT_OK;
}

SpaResult
spa_format_fixate (SpaFormat *format)
{
  unsigned int i, j;
  SpaProps *props;
  uint32_t mask;

  if (format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  props = &format->props;
  mask = props->unset_mask;

  for (i = 0; i < props->n_prop_info; i++) {
    if (mask & 1) {
      const SpaPropInfo *pi = &props->prop_info[i];

      switch (pi->range_type) {
        case SPA_PROP_RANGE_TYPE_NONE:
          break;
	case SPA_PROP_RANGE_TYPE_MIN_MAX:
          break;
	case SPA_PROP_RANGE_TYPE_STEP:
          break;
	case SPA_PROP_RANGE_TYPE_ENUM:
        {
          for (j = 0; j < pi->n_range_values; j++) {
            const SpaPropRangeInfo *ri = &pi->range_values[j];
            memcpy (SPA_MEMBER (props, pi->offset, void), ri->value, ri->size);
            SPA_PROPS_INDEX_SET (props, i);
            break;
          }
          break;
        }
	case SPA_PROP_RANGE_TYPE_FLAGS:
          break;
        default:
          break;
      }
    }
    mask >>= 1;
  }
  return SPA_RESULT_OK;
}
