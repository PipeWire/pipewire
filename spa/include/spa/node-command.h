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

#ifndef __SPA_NODE_COMMAND_H__
#define __SPA_NODE_COMMAND_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaNodeCommand SpaNodeCommand;

#include <spa/defs.h>
#include <spa/clock.h>

typedef enum {
  SPA_NODE_COMMAND_INVALID                 =  0,
  SPA_NODE_COMMAND_PAUSE,
  SPA_NODE_COMMAND_START,
  SPA_NODE_COMMAND_FLUSH,
  SPA_NODE_COMMAND_DRAIN,
  SPA_NODE_COMMAND_MARKER,
  SPA_NODE_COMMAND_CLOCK_UPDATE
} SpaNodeCommandType;

struct _SpaNodeCommand {
  SpaNodeCommandType type;
  void              *data;
  size_t             size;
};

/**
 * SpaNodeCommandClockUpdate:
 * @change_mask: marks which fields are updated
 * @rate: the number of  @ticks per second
 * @ticks: the new ticks, when @change_mask = 1<<0
 * @monotonic_time: the new monotonic time in nanoseconds associated with
 *                  @ticks, when @change_mask = 1<<0
 * @offset: the difference between the time when this update was generated
 *          and @monotonic_time in nanoseconds
 * @scale: update to the speed stored as Q16.16, @change_mask = 1<<1
 * @state: the new clock state, when @change_mask = 1<<2
 */
typedef struct {
#define SPA_NODE_COMMAND_CLOCK_UPDATE_TIME        (1 << 0)
#define SPA_NODE_COMMAND_CLOCK_UPDATE_SCALE       (1 << 1)
#define SPA_NODE_COMMAND_CLOCK_UPDATE_STATE       (1 << 2)
#define SPA_NODE_COMMAND_CLOCK_UPDATE_LATENCY     (1 << 3)
  uint32_t      change_mask;
  int32_t       rate;
  int64_t       ticks;
  int64_t       monotonic_time;
  int64_t       offset;
  int32_t       scale;
  SpaClockState state;
#define SPA_NODE_COMMAND_CLOCK_UPDATE_FLAG_LIVE   (1 << 0)
  uint32_t      flags;
  int64_t       latency;
} SpaNodeCommandClockUpdate;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_COMMAND_H__ */
