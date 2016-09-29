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

#include <glib.h>

#include "pinos/client/pinos.h"

const gchar g_log_domain_pinos[] = "Pinos";

GQuark
pinos_error_quark (void)
{
  static GQuark quark = 0;
  if (!quark)
    quark = g_quark_from_static_string ("pinos-error-quark");
  return quark;
}


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

/**
 * pinos_client_name:
 *
 * Make a new pinos client name that can be used to construct a context.
 */
gchar *
pinos_client_name (void)
{
  const char *c;

  if ((c = g_get_application_name ()))
    return g_strdup (c);
  else if ((c = g_get_prgname ()))
    return g_strdup (c);
  else
    return g_strdup_printf ("pinos-pid-%lu", (gulong) getpid ());
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
  g_return_if_fail (properties != NULL);

  if (!pinos_properties_get (properties, "application.name"))
    pinos_properties_set (properties, "application.name", g_get_application_name ());

  if (!pinos_properties_get (properties, "application.prgname"))
    pinos_properties_set (properties, "application.prgname", g_get_prgname ());

  if (!pinos_properties_get (properties, "application.language")) {
    const gchar *str = g_getenv ("LANG");
    if (str)
      pinos_properties_set (properties, "application.language", str);
  }
  if (!pinos_properties_get (properties, "application.process.id")) {
    gchar *str = g_strdup_printf ("%lu", (gulong) getpid());
    pinos_properties_set (properties, "application.process.id", str);
    g_free (str);
  }
  if (!pinos_properties_get (properties, "application.process.user"))
    pinos_properties_set (properties, "application.process.user", g_get_user_name ());

  if (!pinos_properties_get (properties, "application.process.host"))
    pinos_properties_set (properties, "application.process.host", g_get_host_name ());

  if (!pinos_properties_get (properties, "application.process.session_id")) {
    const gchar *str = g_getenv ("XDG_SESSION_ID");
    if (str)
      pinos_properties_set (properties, "application.process.session_id", str);
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
  g_return_if_fail (properties != NULL);
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
