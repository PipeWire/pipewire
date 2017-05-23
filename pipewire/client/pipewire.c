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
#include <stdio.h>
#include <sys/prctl.h>
#include <pwd.h>

#include "pipewire/client/pipewire.h"

/**
 * pw_init:
 * @argc: pointer to argc
 * @argv: pointer to argv
 *
 * initialize the PipeWire system, parse and modify any parameters given
 * by @argc and @argv.
 */
void
pw_init (int *argc, char **argv[])
{
  const char *str;

  if ((str = getenv ("PIPEWIRE_DEBUG")))
    pw_log_set_level (atoi (str));
}

const char *
pw_get_application_name (void)
{
  return NULL;
}

const char *
pw_get_prgname (void)
{
  static char tcomm[16+1];
  spa_zero(tcomm);

  if (prctl (PR_GET_NAME, (unsigned long) tcomm, 0, 0, 0) == 0)
    return tcomm;

  return NULL;
}

const char *
pw_get_user_name (void)
{
  struct passwd *pw;

  if ((pw = getpwuid (getuid ())))
    return pw->pw_name;

  return NULL;
}

const char *
pw_get_host_name (void)
{
  static char hname[256];

  if (gethostname (hname, 256) < 0)
    return NULL;

  hname[255] = 0;
  return hname;
}

/**
 * pw_client_name:
 *
 * Make a new PipeWire client name that can be used to construct a context.
 */
char *
pw_client_name (void)
{
  char *c;
  const char *cc;

  if ((cc = pw_get_application_name ()))
    return strdup (cc);
  else if ((cc = pw_get_prgname ()))
    return strdup (cc);
  else {
    asprintf (&c, "pipewire-pid-%zd", (size_t) getpid ());
    return c;
  }
}

/**
 * pw_fill_context_properties:
 * @properties: a #struct pw_properties
 *
 * Fill @properties with a set of default context properties.
 */
void
pw_fill_context_properties (struct pw_properties *properties)
{
  if (!pw_properties_get (properties, "application.name"))
    pw_properties_set (properties, "application.name", pw_get_application_name ());

  if (!pw_properties_get (properties, "application.prgname"))
    pw_properties_set (properties, "application.prgname", pw_get_prgname ());

  if (!pw_properties_get (properties, "application.language")) {
    pw_properties_set (properties, "application.language", getenv ("LANG"));
  }
  if (!pw_properties_get (properties, "application.process.id")) {
    pw_properties_setf (properties, "application.process.id", "%zd", (size_t) getpid ());
  }
  if (!pw_properties_get (properties, "application.process.user"))
    pw_properties_set (properties, "application.process.user", pw_get_user_name ());

  if (!pw_properties_get (properties, "application.process.host"))
    pw_properties_set (properties, "application.process.host", pw_get_host_name ());

  if (!pw_properties_get (properties, "application.process.session_id")) {
    pw_properties_set (properties, "application.process.session_id", getenv ("XDG_SESSION_ID"));
  }
}

/**
 * pw_fill_stream_properties
 * @properties: a #struct pw_properties
 *
 * Fill @properties with a set of default stream properties.
 */
void
pw_fill_stream_properties (struct pw_properties *properties)
{
}

enum pw_direction
pw_direction_reverse (enum pw_direction direction)
{
  if (direction == PW_DIRECTION_INPUT)
    return PW_DIRECTION_OUTPUT;
  else if (direction == PW_DIRECTION_OUTPUT)
    return PW_DIRECTION_INPUT;
  return direction;
}
