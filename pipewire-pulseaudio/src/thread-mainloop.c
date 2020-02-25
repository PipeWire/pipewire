/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#include <pipewire/log.h>
#include <pipewire/thread-loop.h>

#include <pulse/mainloop.h>
#include <pulse/thread-mainloop.h>

#include "internal.h"

struct pa_threaded_mainloop
{
	pa_mainloop *loop;
	struct pw_thread_loop *tloop;
};

SPA_EXPORT
pa_threaded_mainloop *pa_threaded_mainloop_new(void)
{
	pa_threaded_mainloop *m;

	m = calloc(1, sizeof(pa_threaded_mainloop));
	if (m == NULL)
		return NULL;

	m->loop = pa_mainloop_new();
	if (m->loop == NULL)
		goto no_mem;

	m->tloop = pw_thread_loop_new_full(m->loop->loop, "pipewire-pulse", NULL);
	if (m->tloop == NULL)
		goto no_mem;

	return m;

     no_mem:
	if (m->loop)
		pa_mainloop_free(m->loop);
	free(m);
	return NULL;
}

SPA_EXPORT
void pa_threaded_mainloop_free(pa_threaded_mainloop* m)
{
	spa_return_if_fail(m != NULL);
	pw_thread_loop_destroy(m->tloop);
	pa_mainloop_free(m->loop);
	free(m);
}

SPA_EXPORT
int pa_threaded_mainloop_start(pa_threaded_mainloop *m)
{
	spa_return_val_if_fail(m != NULL, -EINVAL);
	return pw_thread_loop_start(m->tloop);
}

SPA_EXPORT
void pa_threaded_mainloop_stop(pa_threaded_mainloop *m)
{
	spa_return_if_fail(m != NULL);
	pw_thread_loop_stop(m->tloop);
}

SPA_EXPORT
void pa_threaded_mainloop_lock(pa_threaded_mainloop *m)
{
	spa_return_if_fail(m != NULL);
	pw_thread_loop_lock(m->tloop);
}

SPA_EXPORT
void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m)
{
	spa_return_if_fail(m != NULL);
	pw_thread_loop_unlock(m->tloop);
}

SPA_EXPORT
void pa_threaded_mainloop_wait(pa_threaded_mainloop *m)
{
	spa_return_if_fail(m != NULL);
	pw_thread_loop_wait(m->tloop);
}

SPA_EXPORT
void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_accept)
{
	spa_return_if_fail(m != NULL);
	pw_thread_loop_signal(m->tloop, wait_for_accept);
}

SPA_EXPORT
void pa_threaded_mainloop_accept(pa_threaded_mainloop *m)
{
	spa_return_if_fail(m != NULL);
	pw_thread_loop_accept(m->tloop);
}

SPA_EXPORT
int pa_threaded_mainloop_get_retval(PA_CONST pa_threaded_mainloop *m)
{
	spa_return_val_if_fail(m != NULL, -EINVAL);
	return pa_mainloop_get_retval(m->loop);
}

SPA_EXPORT
pa_mainloop_api* pa_threaded_mainloop_get_api(pa_threaded_mainloop*m)
{
	spa_return_val_if_fail(m != NULL, NULL);
	return pa_mainloop_get_api(m->loop);
}

SPA_EXPORT
int pa_threaded_mainloop_in_thread(pa_threaded_mainloop *m)
{
	spa_return_val_if_fail(m != NULL, -EINVAL);
	return pw_thread_loop_in_thread(m->tloop);
}

SPA_EXPORT
void pa_threaded_mainloop_set_name(pa_threaded_mainloop *m, const char *name)
{
	spa_return_if_fail(m != NULL);
	spa_return_if_fail(name != NULL);
}
