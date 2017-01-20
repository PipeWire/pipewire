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

#include <spa/lib/props.h>

#include <pinos/client/utils.h>
#include <pinos/server/core.h>
#include <pinos/server/module.h>

#include "spa-monitor.h"
#include "spa-node.h"

static SpaResult
setup_video_node (SpaNode *spa_node, PinosProperties *pinos_props) {
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;
  const char *pattern;
  uint32_t pattern_int;

  /* Retrieve pattern property */
  pattern = pinos_properties_get (pinos_props, "pattern");
  if (strcmp (pattern, "smpte-snow") == 0) {
    pattern_int = 0;
  } else if (strcmp (pattern, "snow") == 0) {
    pattern_int = 1;
  } else {
    pinos_log_debug ("Unrecognized pattern");
    return SPA_RESULT_ERROR;
  }

  value.value = &pattern_int;
  value.size = sizeof(uint32_t);

  if ((res = spa_node_get_props (spa_node, &props)) != SPA_RESULT_OK) {
    pinos_log_debug ("spa_node_get_props failed: %d", res);
    return SPA_RESULT_ERROR;
  }

  spa_props_set_value (props, spa_props_index_for_name (props, "pattern"), &value);

  if ((res = spa_node_set_props (spa_node, props)) != SPA_RESULT_OK) {
    pinos_log_debug ("spa_node_set_props failed: %d", res);
    return SPA_RESULT_ERROR;
  }

  return SPA_RESULT_OK;
}

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
    pinos_free_strv (tmp_argv);
  }

  pinos_spa_monitor_load (module->core,
                          "build/spa/plugins/alsa/libspa-alsa.so",
                          "alsa-monitor",
                          "alsa");
  pinos_spa_monitor_load (module->core,
                          "build/spa/plugins/v4l2/libspa-v4l2.so",
                          "v4l2-monitor",
                          "v4l2");
  pinos_spa_node_load (module->core,
                       "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so",
                       "audiotestsrc",
                       "audiotestsrc",
                       NULL,
                       NULL);
  pinos_spa_node_load (module->core,
                       "build/spa/plugins/videotestsrc/libspa-videotestsrc.so",
                       "videotestsrc",
                       "videotestsrc",
                       video_props,
                       setup_video_node);

  return true;
}
