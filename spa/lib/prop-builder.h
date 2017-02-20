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

#ifndef __SPA_PROP_BUILDER_H__
#define __SPA_PROP_BUILDER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/props.h>

typedef struct SpaPropBuilderRange SpaPropBuilderRange;
typedef struct SpaPropBuilderInfo SpaPropBuilderInfo;
typedef struct SpaPropBuilder SpaPropBuilder;

struct SpaPropBuilderRange {
  SpaPropBuilderRange *next;
  SpaPropRangeInfo     info;
};

struct SpaPropBuilderInfo {
  SpaPropBuilderInfo  *next;
  SpaPropInfo          info;
  const void          *value;
  SpaPropBuilderRange *ranges;
};

struct SpaPropBuilder {
  SpaPropBuilderInfo  *head;
  SpaPropBuilderInfo  *info;
  SpaPropBuilderRange *range;
  size_t               n_prop_info;
  size_t               n_range_info;
  size_t               size;
  size_t               struct_size;
  off_t                prop_offset;
  void                *dest;
  void                *userdata;
  void   (*finish)    (SpaPropBuilder *b);
};


void   spa_prop_builder_init       (SpaPropBuilder      *b,
                                    size_t               struct_size,
                                    off_t                prop_offset);

void   spa_prop_builder_add_info   (SpaPropBuilder      *b,
                                    SpaPropBuilderInfo  *i);
void   spa_prop_builder_add_range  (SpaPropBuilder      *b,
                                    SpaPropBuilderRange *r);

void * spa_prop_builder_finish     (SpaPropBuilder      *b);

SpaResult       spa_props_filter      (SpaPropBuilder *b,
                                       const SpaProps *props,
                                       const SpaProps *filter);

#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPA_PROP_BUILDER_H__ */
