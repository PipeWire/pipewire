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

#define SPA_TYPE__Clock		SPA_TYPE_INTERFACE_BASE "Clock"
#define SPA_TYPE_CLOCK_BASE	SPA_TYPE__Clock ":"

/**
 * spa_clock_state:
 * @SPA_CLOCK_STATE_STOPPED: the clock is stopped
 * @SPA_CLOCK_STATE_PAUSED: the clock is paused
 * @SPA_CLOCK_STATE_RUNNING: the clock is running
 */
enum spa_clock_state {
	SPA_CLOCK_STATE_STOPPED,
	SPA_CLOCK_STATE_PAUSED,
	SPA_CLOCK_STATE_RUNNING,
};

#include <spa/utils/defs.h>
#include <spa/pod/builder.h>

/**
 * spa_clock:
 *
 * A time provider.
 */
struct spa_clock {
	/* the version of this clock. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_CLOCK	0
	uint32_t version;

	const struct spa_dict *info;
	/**
	 * spa_clock::state:
	 *
	 * The current state of the clock
	 */
	enum spa_clock_state state;
	/**
	 * spa_clock::get_props:
	 * @clock: a #spa_clock
	 *
	 * Get the parameters of @clock.
	 *
	 * The returned @props is a snapshot of the current configuration and
	 * can be modified. The modifications will take effect after a call
	 * to spa_clock::set_props.
	 *
	 * Returns: 0 on success
	 *          -EINVAL when clock or props are %NULL
	 *          -ENOTSUP when there are no properties
	 *                 implemented on @clock
	 */
	int (*enum_params) (struct spa_clock *clock,
			    uint32_t id, uint32_t *index,
			    struct spa_pod_builder *builder);
	/**
	 * spa_clock::set_props:
	 * @clock: a #spa_clock
	 * @props: a #struct spa_props
	 *
	 * Set the configurable properties in @clock.
	 *
	 * Usually, @props will be obtained from spa_clock::get_props and then
	 * modified but it is also possible to set another #struct spa_props object
	 * as long as its keys and types match those of struct spa_props::get_props.
	 *
	 * Properties with keys that are not known are ignored.
	 *
	 * If @props is NULL, all the properties are reset to their defaults.
	 *
	 * Returns: 0 on success
	 *          -EINVAL when clock is %NULL
	 *          -ENOTSUP when no properties can be
	 *                 modified on @clock.
	 *          -ENOENT when the param with id is not supported
	 */
	int (*set_param) (struct spa_clock *clock,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod_object *param);

	int (*get_time) (struct spa_clock *clock,
			 int32_t *rate,
			 int64_t *ticks,
			 int64_t *monotonic_time);
};

#define spa_clock_enum_params(n,...)	(n)->enum_params((n),__VA_ARGS__)
#define spa_clock_set_param(n,...)	(n)->set_param((n),__VA_ARGS__)
#define spa_clock_get_time(n,...)	(n)->get_time((n),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif				/* __SPA_CLOCK_H__ */
