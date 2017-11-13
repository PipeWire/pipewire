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

/** The state of the clock */
enum spa_clock_state {
	SPA_CLOCK_STATE_STOPPED,	/*< the clock is stopped */
	SPA_CLOCK_STATE_PAUSED,		/*< the clock is paused */
	SPA_CLOCK_STATE_RUNNING,	/*< the clock is running */
};

#include <spa/utils/defs.h>
#include <spa/pod/builder.h>

/**
 * A time provider.
 */
struct spa_clock {
	/* the version of this clock. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_CLOCK	0
	uint32_t version;

	/** extra clock information */
	const struct spa_dict *info;

	/** The current state of the clock */
	enum spa_clock_state state;
	/**
	 * Get the parameters of \a clock.
	 *
	 * The returned \a props is a snapshot of the current configuration and
	 * can be modified. The modifications will take effect after a call
	 * to set_props.
	 *
	 * \param clock a spa_clock
	 * \param id the paramter id to enumerate
	 * \param index state while iterating, 0 for the first param
	 * \param param result parameter
	 * \param builder builder for \a param
	 * \return 1 on success
	 *         0 when no more items are available
	 *         -EINVAL when invalid parameters are provided
	 *         -ENOTSUP when there are no parameters
	 *                 implemented on \a clock
	 *         -ENOENT when the param with id is not supported
	 */
	int (*enum_params) (struct spa_clock *clock,
			    uint32_t id, uint32_t *index,
			    struct spa_pod **param,
			    struct spa_pod_builder *builder);
	/**
	 * Set the configurable properties in \a clock.
	 *
	 * Usually, \a props will be obtained from spa_clock::get_props and then
	 * modified but it is also possible to set another #struct spa_props object
	 * as long as its keys and types match those of struct spa_props::get_props.
	 *
	 * Properties with keys that are not known are ignored.
	 *
	 * If \a props is NULL, all the properties are reset to their defaults.
	 *
	 * \param clock a spa_clock
	 * \param id the paramter id to configure
	 * \param flags extra parameter flags
	 * \param param the parameter to configure
	 * \return 0 on success
	 *         -EINVAL when invalid parameters are provided
	 *         -ENOTSUP when no parameters can be
	 *                 modified on \a clock.
	 *         -ENOENT when the param with id is not supported
	 */
	int (*set_param) (struct spa_clock *clock,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param);

	/** Get the time of \a clock
	 *
	 * Get an atomic snapshot between the time of the monotonic clock and
	 * the clock specific time expressed in ticks.
	 *
	 * \param clock the clock
	 * \param rate result rate. This is the number of ticks per second
	 * \param ticks result number of ticks. There are \a rate ticks per second.
	 * \param monotonic_time the time of the monotonic clock when \a ticks was
	 *        obtained.
	 */
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
