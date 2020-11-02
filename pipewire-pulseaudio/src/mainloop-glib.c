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

#include <errno.h>

#include <spa/utils/result.h>

#include <pipewire/log.h>
#include <pipewire/loop.h>

#include <pulse/glib-mainloop.h>
#include <pulse/mainloop.h>

#include "internal.h"

struct source {
	GSource base;
	struct pw_loop *loop;
};

struct pa_glib_mainloop {
	GMainContext *context;
	pa_mainloop *loop;
	struct source *source;
	guint id;
};

static gboolean source_prepare (GSource *base, int *timeout)
{
	*timeout = -1;
	return FALSE;
}

static gboolean source_dispatch (GSource *source, GSourceFunc callback, gpointer user_data)
{
	struct source *s = (struct source *) source;
	int result;

	pw_loop_enter (s->loop);
	do {
		result = pw_loop_iterate (s->loop, 0);
	} while (result == -EINTR || result == -EAGAIN);
	pw_loop_leave (s->loop);

	if (result < 0)
		g_warning ("pipewire_loop_iterate failed: %s", spa_strerror (result));

	return TRUE;
}

static GSourceFuncs source_funcs =
{
	source_prepare,
	NULL,
	source_dispatch,
	NULL,
	NULL,
	NULL,
};

SPA_EXPORT
pa_glib_mainloop *pa_glib_mainloop_new(GMainContext *c)
{

	pa_glib_mainloop *loop;

	loop = calloc(1, sizeof(pa_glib_mainloop));
	if (loop == NULL)
		goto error;

	loop->context = c;
	loop->loop = pa_mainloop_new();
	if (loop->loop == NULL)
		goto error_free;

	loop->source = (struct source *) g_source_new(&source_funcs, sizeof(struct source));
	loop->source->loop = loop->loop->loop;

	g_source_add_unix_fd (&loop->source->base,
                        pw_loop_get_fd(loop->source->loop),
                        G_IO_IN | G_IO_ERR);

	loop->id = g_source_attach (&loop->source->base, loop->context);

	return loop;

      error_free:
	free(loop);
      error:
	return NULL;

}

SPA_EXPORT
void pa_glib_mainloop_free(pa_glib_mainloop* g)
{
	g_source_destroy(&g->source->base);
	pa_mainloop_free(g->loop);
	free(g);
}

SPA_EXPORT
pa_mainloop_api* pa_glib_mainloop_get_api(pa_glib_mainloop *g)
{
	return pa_mainloop_get_api(g->loop);
}
