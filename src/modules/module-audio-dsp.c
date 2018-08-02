/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include "pipewire/core.h"
#include "pipewire/interfaces.h"
#include "pipewire/log.h"
#include "pipewire/module.h"

#include "module-media-session/audio-dsp.h"

static const struct spa_dict_item module_props[] = {
	{ PW_MODULE_PROP_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_MODULE_PROP_DESCRIPTION, "Manage audio DSP nodes" },
	{ PW_MODULE_PROP_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_factory *this;
	struct pw_properties *properties;

	struct pw_module *module;
	struct spa_hook module_listener;
};

struct resource_data {
	struct factory_data *data;

	struct pw_resource *resource;
	struct spa_hook resource_listener;

	struct pw_node *dsp;
};

static void resource_destroy(void *data)
{
	struct resource_data *d = data;
	if (d->dsp)
		pw_node_destroy(d->dsp);
}

static struct pw_resource_events resource_events =
{
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = _data;
	struct resource_data *rd;
	struct pw_resource *node_resource;
	struct pw_client *client;
	int channels, rate, maxbuffer;
	const char *str;
	enum pw_direction direction;

	if (resource == NULL)
		goto no_resource;

	client = pw_resource_get_client(resource);

	node_resource = pw_resource_new(client,
					new_id, PW_PERM_RWX, type, version,
					sizeof(struct resource_data));
	if (node_resource == NULL)
		goto no_mem;

	rd = pw_resource_get_user_data(node_resource);
	rd->data = d;
	rd->resource = node_resource;

	pw_resource_add_listener(node_resource, &rd->resource_listener, &resource_events, rd);

	if ((str = pw_properties_get(properties, "audio-dsp.direction")) == NULL)
		goto no_props;

	direction = pw_properties_parse_int(str);

	if ((str = pw_properties_get(properties, "audio-dsp.channels")) == NULL)
		goto no_props;

	channels = pw_properties_parse_int(str);

	if ((str = pw_properties_get(properties, "audio-dsp.rate")) == NULL)
		goto no_props;

	rate = pw_properties_parse_int(str);

	if ((str = pw_properties_get(properties, "audio-dsp.maxbuffer")) == NULL)
		goto no_props;

	maxbuffer = pw_properties_parse_int(str);

	rd->dsp = pw_audio_dsp_new(pw_module_get_core(d->module),
			properties,
			direction,
			channels, rate, maxbuffer, 0);

	if (rd->dsp == NULL)
		goto no_mem;

	pw_node_register(rd->dsp, client, pw_module_get_global(d->module), NULL);
	pw_node_set_active(rd->dsp, true);

	return rd->dsp;

      no_resource:
	pw_log_error("audio-dsp needs a resource");
	pw_resource_error(resource, -EINVAL, "no resource");
	goto done;
      no_props:
	pw_log_error("audio-dsp needs a property");
	pw_resource_error(resource, -EINVAL, "no property");
	goto done;
      no_mem:
	pw_log_error("can't create node");
	pw_resource_error(resource, -ENOMEM, "no memory");
	goto done;
      done:
	if (properties)
		pw_properties_free(properties);
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;

	spa_hook_remove(&d->module_listener);

	if (d->properties)
		pw_properties_free(d->properties);

	pw_factory_destroy(d->this);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_type *t = pw_core_get_type(core);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "audio-dsp",
				 t->node,
				 PW_VERSION_NODE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -ENOMEM;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->module = module;
	data->properties = properties;

	pw_log_debug("module %p: new", module);

	pw_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_factory_register(factory, NULL, pw_module_get_global(module), NULL);

	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
