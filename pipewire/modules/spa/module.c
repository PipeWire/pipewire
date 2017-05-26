/* PipeWire
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

#include <pipewire/client/utils.h>
#include <pipewire/server/core.h>
#include <pipewire/server/module.h>

#include "spa-monitor.h"
#include "spa-node.h"

static int
setup_video_node(struct pw_core *core, struct spa_node *spa_node, struct pw_properties *pw_props)
{
	int res;
	struct spa_props *props;
	struct spa_pod_prop *prop;
	const char *pattern, *pattern_type;

	/* Retrieve pattern property */
	pattern = pw_properties_get(pw_props, "pattern");
	if (strcmp(pattern, "smpte-snow") == 0) {
		pattern_type = SPA_TYPE_PROPS__patternType ":smpte-snow";
	} else if (strcmp(pattern, "snow") == 0) {
		pattern_type = SPA_TYPE_PROPS__patternType ":snow";
	} else {
		pw_log_debug("Unrecognized pattern");
		return SPA_RESULT_ERROR;
	}

	if ((res = spa_node_get_props(spa_node, &props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_get_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}

	if ((prop =
	     spa_pod_object_find_prop(&props->object,
				      spa_type_map_get_id(core->type.map,
							  SPA_TYPE_PROPS__patternType)))) {
		if (prop->body.value.type == SPA_POD_TYPE_ID)
			SPA_POD_VALUE(struct spa_pod_id, &prop->body.value) =
			    spa_type_map_get_id(core->type.map, pattern_type);
	}

	if ((res = spa_node_set_props(spa_node, props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_set_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}

	return SPA_RESULT_OK;
}

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	struct pw_properties *video_props = NULL, *audio_props = NULL;

	if (args != NULL) {
		char **tmp_argv;
		char **argv;
		int n_tokens;
		int opt = 0;

		tmp_argv = pw_split_strv(args, " \t", INT_MAX, &n_tokens);

		argv = malloc((n_tokens + 1) * sizeof(char *));
		/* getopt expects name of executable on the first place of argv */
		argv[0] = "videotestsrc";

		for (int i = 1; i <= n_tokens; i++) {
			argv[i] = tmp_argv[i - 1];
		}

		video_props = pw_properties_new("media.class", "Video/Source", NULL);

		static struct option long_options[] = {
			{"filter", required_argument, 0, 'f'},
			{"pattern", required_argument, 0, 'p'},
			{"resolution", required_argument, 0, 'r'},
			{0, 0, 0, 0}
		};

		while ((opt = getopt_long(n_tokens + 1, argv, "p:r:f:", long_options, NULL)) != -1) {
			switch (opt) {
			case 'f':
				pw_properties_set(video_props, "filter", optarg);
				break;
			case 'p':
				pw_properties_set(video_props, "pattern", optarg);
				break;
			case 'r':
				pw_properties_set(video_props, "resolution", optarg);
				break;
			default:
				break;
			}
		}
		free(argv);
		pw_free_strv(tmp_argv);
	}

	pw_spa_monitor_load(module->core,
			    "build/spa/plugins/alsa/libspa-alsa.so", "alsa-monitor", "alsa");
	pw_spa_monitor_load(module->core,
			    "build/spa/plugins/v4l2/libspa-v4l2.so", "v4l2-monitor", "v4l2");
	audio_props = pw_properties_new("media.class", "Audio/Source", NULL);
	pw_spa_node_load(module->core,
			 "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so",
			 "audiotestsrc", "audiotestsrc", audio_props, NULL);
	pw_spa_node_load(module->core,
			 "build/spa/plugins/videotestsrc/libspa-videotestsrc.so",
			 "videotestsrc", "videotestsrc", video_props, setup_video_node);

	return true;
}
