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

#include <server/core.h>
#include <server/module.h>

#include "spa-monitor.h"
#include "spa-node.h"

bool
pinos__module_init (PinosModule * module, const char * args)
{
  pinos_spa_monitor_load (module->core, "build/spa/plugins/alsa/libspa-alsa.so", "alsa-monitor", args);
  pinos_spa_monitor_load (module->core, "build/spa/plugins/v4l2/libspa-v4l2.so", "v4l2-monitor", args);
  pinos_spa_node_load (module->core,
                       "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so",
                       "audiotestsrc",
                       "audiotestsrc",
                       NULL, args);
  pinos_spa_node_load (module->core,
                       "build/spa/plugins/videotestsrc/libspa-videotestsrc.so",
                       "videotestsrc",
                       "videotestsrc",
                       NULL, args);

  return TRUE;
}
