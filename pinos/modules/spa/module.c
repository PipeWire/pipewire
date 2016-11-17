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

#include <getopt.h>
#include <limits.h>

#include <client/utils.h>
#include <server/core.h>
#include <server/module.h>

#include "spa-monitor.h"
#include "spa-node.h"

bool
pinos__module_init (PinosModule * module, const char * args)
{
  PinosProperties *video_props = NULL;

  if (args != NULL) {
    char **tmp_argv;
    char **argv;
    int n_tokens;
    int opt = 0;

    tmp_argv = pinos_split_strv (args, " \t", INT_MAX, &n_tokens);

    argv = malloc ((n_tokens+1) * sizeof (char *));
    /* getopt expects name of executable on the first place of argv */
    argv[0] = "videotestsrc";

    for (int i = 1; i <= n_tokens; i++) {
      argv[i] = tmp_argv[i-1];
    }

    video_props = pinos_properties_new (NULL, NULL);

    static struct option long_options[] = {
      {"filter",      required_argument,  0,  'f' },
      {"pattern",     required_argument,  0,  'p' },
      {"resolution",  required_argument,  0,  'r' },
      {0,             0,                  0,  0   }
    };

    while ((opt = getopt_long (n_tokens+1, argv, "p:r:f:", long_options, NULL)) != -1) {
      switch (opt) {
        case 'f':
          pinos_properties_set (video_props, "filter", optarg);
          break;
        case 'p':
          pinos_properties_set (video_props, "pattern", optarg);
          break;
        case 'r':
          pinos_properties_set (video_props, "resolution", optarg);
          break;
        default:
          break;
      }
    }
    free (argv);
  }

  pinos_spa_monitor_load (module->core, "build/spa/plugins/alsa/libspa-alsa.so", "alsa-monitor");
  pinos_spa_monitor_load (module->core, "build/spa/plugins/v4l2/libspa-v4l2.so", "v4l2-monitor");
  pinos_spa_node_load (module->core,
                       "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so",
                       "audiotestsrc",
                       "audiotestsrc",
                       NULL);
  pinos_spa_node_load (module->core,
                       "build/spa/plugins/videotestsrc/libspa-videotestsrc.so",
                       "videotestsrc",
                       "videotestsrc",
                       video_props);

  return TRUE;
}
