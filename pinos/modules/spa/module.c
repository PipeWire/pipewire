/* Pinos
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
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

#include <server/daemon.h>
#include <server/module.h>

#include "spa-alsa-sink.h"
#include "spa-v4l2-source.h"
#include "spa-audiotestsrc.h"

gboolean pinos__module_init (PinosModule *module, const gchar * args);

G_MODULE_EXPORT gboolean
pinos__module_init (PinosModule * module, G_GNUC_UNUSED const gchar * args)
{
  pinos_spa_alsa_sink_new (module->daemon, "alsa-sink", NULL);
  pinos_spa_v4l2_source_new (module->daemon, "v4l2-source", NULL);
  pinos_spa_audiotestsrc_new (module->daemon, "audiotestsrc", NULL);

  return TRUE;
}
