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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <getopt.h>
#include <limits.h>

#include <spa/lib/props.h>

#include <pipewire/client/utils.h>
#include <pipewire/server/core.h>
#include <pipewire/server/module.h>

#include "spa-monitor.h"
#include "spa-node.h"

static int
setup_props(struct pw_core *core, struct spa_node *spa_node, struct pw_properties *pw_props)
{
	int res;
	struct spa_props *props;
	void *state = NULL;
	const char *key;

	if ((res = spa_node_get_props(spa_node, &props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_get_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}

	while ((key = pw_properties_iterate(pw_props, &state))) {
		struct spa_pod_prop *prop;
		uint32_t id;

		if (!spa_type_is_a(key, SPA_TYPE_PROPS_BASE))
			continue;

		id = spa_type_map_get_id(core->type.map, key);
		if (id == SPA_ID_INVALID)
			continue;

		if ((prop = spa_pod_object_find_prop(&props->object, id))) {
			const char *value = pw_properties_get(pw_props, key);

			pw_log_info("configure prop %s", key);

			switch(prop->body.value.type) {
			case SPA_POD_TYPE_ID:
				SPA_POD_VALUE(struct spa_pod_id, &prop->body.value) =
					spa_type_map_get_id(core->type.map, value);
				break;
			case SPA_POD_TYPE_INT:
				SPA_POD_VALUE(struct spa_pod_int, &prop->body.value) = atoi(value);
				break;
			case SPA_POD_TYPE_LONG:
				SPA_POD_VALUE(struct spa_pod_long, &prop->body.value) = atoi(value);
				break;
			case SPA_POD_TYPE_FLOAT:
				SPA_POD_VALUE(struct spa_pod_float, &prop->body.value) = atof(value);
				break;
			case SPA_POD_TYPE_DOUBLE:
				SPA_POD_VALUE(struct spa_pod_double, &prop->body.value) = atof(value);
				break;
			case SPA_POD_TYPE_STRING:
				break;
			default:
				break;
			}
		}
	}

	if ((res = spa_node_set_props(spa_node, props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_set_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}

	return SPA_RESULT_OK;
}

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	const char *dir;
	struct pw_properties *props = NULL;
	char **argv;
	int i, n_tokens;

	if (args == NULL)
		goto wrong_arguments;

	argv = pw_split_strv(args, " \t", INT_MAX, &n_tokens);
	if (n_tokens < 3)
		goto not_enough_arguments;

	if ((dir = getenv("SPA_PLUGIN_DIR")) == NULL)
		dir = PLUGINDIR;

	props = pw_properties_new(NULL, NULL);

	for (i = 3; i < n_tokens; i++) {
		char **prop;
		int n_props;

		prop = pw_split_strv(argv[i], "=", INT_MAX, &n_props);
		if (n_props >= 2)
			pw_properties_set(props, prop[0], prop[1]);

		pw_free_strv(prop);
	}

	pw_spa_node_load(module->core, dir, argv[0], argv[1], argv[2], props, setup_props);

	pw_free_strv(argv);

	return true;

      not_enough_arguments:
	pw_free_strv(argv);
      wrong_arguments:
	pw_log_error("usage: module-spa-node <plugin> <factory> <name> [key=value ...]");
	return false;
}
