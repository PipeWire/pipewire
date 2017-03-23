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

#ifndef __SPA_CLOCK_H__
#define __SPA_CLOCK_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaClock SpaClock;

#define SPA_TYPE__Clock            "Spa:Interface:Clock"
#define SPA_TYPE_CLOCK_BASE        SPA_TYPE__Clock ":"

/**
 * SpaClockState:
 * @SPA_CLOCK_STATE_STOPPED: the clock is stopped
 * @SPA_CLOCK_STATE_PAUSED: the clock is paused
 * @SPA_CLOCK_STATE_RUNNING: the clock is running
 */
typedef enum {
  SPA_CLOCK_STATE_STOPPED,
  SPA_CLOCK_STATE_PAUSED,
  SPA_CLOCK_STATE_RUNNING,
} SpaClockState;

#include <spa/defs.h>
#include <spa/plugin.h>
#include <spa/props.h>

/**
 * SpaClock:
 *
 * A time provider.
 */
struct _SpaClock {
  /* the total size of this clock. This can be used to expand this
   * structure in the future */
  size_t size;
  /**
   * SpaClock::info
   *
   * Extra information about the clock
   */
  const SpaDict *info;
  /**
   * SpaClock::state:
   *
   * The current state of the clock
   */
  SpaClockState state;
  /**
   * SpaClock::get_props:
   * @clock: a #SpaClock
   * @props: a location for a #SpaProps pointer
   *
   * Get the configurable properties of @clock.
   *
   * The returned @props is a snapshot of the current configuration and
   * can be modified. The modifications will take effect after a call
   * to SpaClock::set_props.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when clock or props are %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when there are no properties
   *                 implemented on @clock
   */
  SpaResult   (*get_props)            (SpaClock          *clock,
                                       SpaProps        **props);
  /**
   * SpaClock::set_props:
   * @clock: a #SpaClock
   * @props: a #SpaProps
   *
   * Set the configurable properties in @clock.
   *
   * Usually, @props will be obtained from SpaClock::get_props and then
   * modified but it is also possible to set another #SpaProps object
   * as long as its keys and types match those of SpaProps::get_props.
   *
   * Properties with keys that are not known are ignored.
   *
   * If @props is NULL, all the properties are reset to their defaults.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when clock is %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when no properties can be
   *                 modified on @clock.
   *          #SPA_RESULT_WRONG_PROPERTY_TYPE when a property has the wrong
   *                 type.
   */
  SpaResult   (*set_props)           (SpaClock         *clock,
                                      const SpaProps   *props);

  SpaResult   (*get_time)            (SpaClock         *clock,
                                      int32_t          *rate,
                                      int64_t          *ticks,
                                      int64_t          *monotonic_time);
};

#define spa_clock_get_props(n,...)          (n)->get_props((n),__VA_ARGS__)
#define spa_clock_set_props(n,...)          (n)->set_props((n),__VA_ARGS__)
#define spa_clock_get_time(n,...)           (n)->get_time((n),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_CLOCK_H__ */
