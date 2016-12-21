/* Pinos
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

#include "pinos/client/pinos.h"

/**
 * pinos_init:
 * @argc: pointer to argc
 * @argv: pointer to argv
 *
 * initialize the pinos system, parse and modify any parameters given
 * by @argc and @argv.
 */
void
pinos_init (int *argc, char **argv[])
{
}

const char *
pinos_get_application_name (void)
{
  return NULL;
}

const char *
pinos_get_prgname (void)
{
  static char tcomm[16+1];
  spa_zero(tcomm);

  if (prctl (PR_GET_NAME, (unsigned long) tcomm, 0, 0, 0) == 0)
    return tcomm;

  return NULL;
}

const char *
pinos_get_user_name (void)
{
  struct passwd *pw;

  if ((pw = getpwuid (getuid ())))
    return pw->pw_name;

  return NULL;
}

const char *
pinos_get_host_name (void)
{
  static char hname[256];

  if (gethostname (hname, 256) < 0)
    return NULL;

  hname[255] = 0;
  return hname;
}

/**
 * pinos_client_name:
 *
 * Make a new pinos client name that can be used to construct a context.
 */
char *
pinos_client_name (void)
{
  char *c;
  const char *cc;

  if ((cc = pinos_get_application_name ()))
    return strdup (cc);
  else if ((cc = pinos_get_prgname ()))
    return strdup (cc);
  else {
    asprintf (&c, "pinos-pid-%zd", (size_t) getpid ());
    return c;
  }
}

/**
 * pinos_fill_context_properties:
 * @properties: a #PinosProperties
 *
 * Fill @properties with a set of default context properties.
 */
void
pinos_fill_context_properties (PinosProperties *properties)
{
  if (!pinos_properties_get (properties, "application.name"))
    pinos_properties_set (properties, "application.name", pinos_get_application_name ());

  if (!pinos_properties_get (properties, "application.prgname"))
    pinos_properties_set (properties, "application.prgname", pinos_get_prgname ());

  if (!pinos_properties_get (properties, "application.language")) {
    pinos_properties_set (properties, "application.language", getenv ("LANG"));
  }
  if (!pinos_properties_get (properties, "application.process.id")) {
    pinos_properties_setf (properties, "application.process.id", "%zd", (size_t) getpid ());
  }
  if (!pinos_properties_get (properties, "application.process.user"))
    pinos_properties_set (properties, "application.process.user", pinos_get_user_name ());

  if (!pinos_properties_get (properties, "application.process.host"))
    pinos_properties_set (properties, "application.process.host", pinos_get_host_name ());

  if (!pinos_properties_get (properties, "application.process.session_id")) {
    pinos_properties_set (properties, "application.process.session_id", getenv ("XDG_SESSION_ID"));
  }
}

/**
 * pinos_fill_stream_properties
 * @properties: a #PinosProperties
 *
 * Fill @properties with a set of default stream properties.
 */
void
pinos_fill_stream_properties (PinosProperties *properties)
{
}

PinosDirection
pinos_direction_reverse (PinosDirection direction)
{
  if (direction == PINOS_DIRECTION_INPUT)
    return PINOS_DIRECTION_OUTPUT;
  else if (direction == PINOS_DIRECTION_OUTPUT)
    return PINOS_DIRECTION_INPUT;
  return direction;
}
