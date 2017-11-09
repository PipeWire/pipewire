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

#include <pipewire/core.h>
#include <pipewire/log.h>
#include <pipewire/module.h>
#include <pipewire/utils.h>

#include "spa-monitor.h"
#include "spa-node.h"

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	struct pw_properties *props = NULL;
	char **argv;
	int i, n_tokens;

	if (args == NULL)
		goto wrong_arguments;

	argv = pw_split_strv(args, " \t", INT_MAX, &n_tokens);
	if (n_tokens < 3)
		goto not_enough_arguments;

	props = pw_properties_new(NULL, NULL);

	for (i = 3; i < n_tokens; i++) {
		char **prop;
		int n_props;

		prop = pw_split_strv(argv[i], "=", INT_MAX, &n_props);
		if (n_props >= 2)
			pw_properties_set(props, prop[0], prop[1]);

		pw_free_strv(prop);
	}

	pw_spa_node_load(pw_module_get_core(module),
			 NULL,
			 pw_module_get_global(module),
			 argv[0], argv[1], argv[2],
			 PW_SPA_NODE_FLAG_ACTIVATE,
			 props, 0);

	pw_free_strv(argv);

	return true;

      not_enough_arguments:
	pw_free_strv(argv);
      wrong_arguments:
	pw_log_error("usage: module-spa-node <plugin> <factory> <name> [key=value ...]");
	return false;
}
