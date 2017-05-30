/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_SIGNAL_H__
#define __PIPEWIRE_SIGNAL_H__

#include <spa/list.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_signal
 * Signal emission helpers
 */

/** A listener \memberof pw_signal */
struct pw_listener {
	struct spa_list link;		/**< link in the signal listeners */
	void (*notify) (void *);	/**< notify function */
};

/** A signal definition \memberof pw_signal */
#define PW_SIGNAL(name,func)				\
	union {						\
		struct spa_list listeners;		\
		void (*notify) func;			\
	} name;

/** Initialize a signal \memberof pw_signal */
#define pw_signal_init(signal)				\
	spa_list_init(&(signal)->listeners);

/** Add a signal listener \memberof pw_signal */
#define pw_signal_add(signal,listener,func)				\
do {									\
	__typeof__((signal)->notify) n = (func);			\
	(listener)->notify = (void(*)(void *)) n;			\
	spa_list_insert((signal)->listeners.prev, &(listener)->link);	\
} while (false);

/** Remove a signal listener \memberof pw_signal */
static inline void pw_signal_remove(struct pw_listener *listener)
{
	spa_list_remove(&listener->link);
}

/** Emit a signal \memberof pw_signal */
#define pw_signal_emit(signal,...)					\
do {									\
	struct pw_listener *l, *next;					\
	spa_list_for_each_safe(l, next, &(signal)->listeners, link)	\
	((__typeof__((signal)->notify))l->notify)(l,__VA_ARGS__);	\
} while (false);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_SIGNAL_H__ */
