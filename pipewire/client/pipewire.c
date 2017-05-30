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

#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <pwd.h>

#include "pipewire/client/pipewire.h"

static char **categories = NULL;

static void configure_debug(const char *str)
{
	char **level;
	int n_tokens;

	level = pw_split_strv(str, ":", INT_MAX, &n_tokens);
	if (n_tokens > 0)
		pw_log_set_level(atoi(level[0]));

	if (n_tokens > 1)
		categories = pw_split_strv(level[1], ",", INT_MAX, &n_tokens);
}

/** Initialize PipeWire
 *
 * \param argc pointer to argc
 * \param argv pointer to argv
 *
 * Initialize the PipeWire system, parse and modify any parameters given
 * by \a argc and \a argv and set up debugging.
 *
 * The environment variable \a PIPEWIRE_DEBUG
 *
 * \memberof pw_pipewire
 */
void pw_init(int *argc, char **argv[])
{
	const char *str;

	if ((str = getenv("PIPEWIRE_DEBUG")))
		configure_debug(str);
}

/** Check if a debug category is enabled
 *
 * \param name the name of the category to check
 * \return true if enabled
 *
 * Debugging categories can be enabled by using the PIPEWIRE_DEBUG
 * environment variable
 *
 * \memberof pw_pipewire
 */
bool pw_debug_is_category_enabled(const char *name)
{
	int i;

	if (categories == NULL)
		return false;

	for (i = 0; categories[i]; i++) {
		if (strcmp (categories[i], name) == 0)
			return true;
	}
	return false;
}

/** Get the application name \memberof pw_pipewire */
const char *pw_get_application_name(void)
{
	return NULL;
}

/** Get the program name \memberof pw_pipewire */
const char *pw_get_prgname(void)
{
	static char tcomm[16 + 1];
	spa_zero(tcomm);

	if (prctl(PR_GET_NAME, (unsigned long) tcomm, 0, 0, 0) == 0)
		return tcomm;

	return NULL;
}

/** Get the user name \memberof pw_pipewire */
const char *pw_get_user_name(void)
{
	struct passwd *pw;

	if ((pw = getpwuid(getuid())))
		return pw->pw_name;

	return NULL;
}

/** Get the host name \memberof pw_pipewire */
const char *pw_get_host_name(void)
{
	static char hname[256];

	if (gethostname(hname, 256) < 0)
		return NULL;

	hname[255] = 0;
	return hname;
}

/** Get the client name
 *
 * Make a new PipeWire client name that can be used to construct a context.
 *
 * \memberof pw_pipewire
 */
char *pw_get_client_name(void)
{
	char *c;
	const char *cc;

	if ((cc = pw_get_application_name()))
		return strdup(cc);
	else if ((cc = pw_get_prgname()))
		return strdup(cc);
	else {
		asprintf(&c, "pipewire-pid-%zd", (size_t) getpid());
		return c;
	}
}

/** Fill context properties
 * \param properties a \ref pw_properties
 *
 * Fill \a properties with a set of default context properties.
 *
 * \memberof pw_pipewire
 */
void pw_fill_context_properties(struct pw_properties *properties)
{
	if (!pw_properties_get(properties, "application.name"))
		pw_properties_set(properties, "application.name", pw_get_application_name());

	if (!pw_properties_get(properties, "application.prgname"))
		pw_properties_set(properties, "application.prgname", pw_get_prgname());

	if (!pw_properties_get(properties, "application.language")) {
		pw_properties_set(properties, "application.language", getenv("LANG"));
	}
	if (!pw_properties_get(properties, "application.process.id")) {
		pw_properties_setf(properties, "application.process.id", "%zd", (size_t) getpid());
	}
	if (!pw_properties_get(properties, "application.process.user"))
		pw_properties_set(properties, "application.process.user", pw_get_user_name());

	if (!pw_properties_get(properties, "application.process.host"))
		pw_properties_set(properties, "application.process.host", pw_get_host_name());

	if (!pw_properties_get(properties, "application.process.session_id")) {
		pw_properties_set(properties, "application.process.session_id",
				  getenv("XDG_SESSION_ID"));
	}
}

/** Fill stream properties
 * \param properties a \ref pw_properties
 *
 * Fill \a properties with a set of default stream properties.
 *
 * \memberof pw_pipewire
 */
void pw_fill_stream_properties(struct pw_properties *properties)
{
}

/** Reverse the direction \memberof pw_pipewire */
enum pw_direction pw_direction_reverse(enum pw_direction direction)
{
	if (direction == PW_DIRECTION_INPUT)
		return PW_DIRECTION_OUTPUT;
	else if (direction == PW_DIRECTION_OUTPUT)
		return PW_DIRECTION_INPUT;
	return direction;
}
