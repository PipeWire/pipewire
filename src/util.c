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

#include <time.h>

#include <pipewire/log.h>
#include <pipewire/pipewire.h>

#include <pulse/util.h>

#define PA_PATH_SEP_CHAR	'/'

SPA_EXPORT
char *pa_get_user_name(char *s, size_t l)
{
	return strncpy(s, pw_get_user_name(), l);
}


SPA_EXPORT
char *pa_get_host_name(char *s, size_t l)
{
	return strncpy(s, pw_get_host_name(), l);
}

SPA_EXPORT
char *pa_get_fqdn(char *s, size_t l)
{
	return strncpy(s, pw_get_host_name(), l);
}

SPA_EXPORT
char *pa_get_home_dir(char *s, size_t l)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
char *pa_get_binary_name(char *s, size_t l)
{
	return strncpy(s, pw_get_prgname(), l);
}

SPA_EXPORT
char *pa_path_get_filename(const char *p)
{
	char *fn;

	if (!p)
		return NULL;

	if ((fn = strrchr(p, PA_PATH_SEP_CHAR)))
		return fn+1;

	return (char*) p;
}

SPA_EXPORT
int pa_msleep(unsigned long t)
{
    struct timespec ts;

    ts.tv_sec = (time_t) (t / SPA_MSEC_PER_SEC);
    ts.tv_nsec = (long) ((t % SPA_MSEC_PER_SEC) * SPA_NSEC_PER_MSEC);

    return nanosleep(&ts, NULL);
}
