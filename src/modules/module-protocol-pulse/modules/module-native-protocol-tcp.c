/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <pipewire/pipewire.h>

#include "../module.h"
#include "../pulse-server.h"
#include "../server.h"
#include "registry.h"

#define NAME "protocol-tcp"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_native_protocol_tcp_data {
	struct module *module;
	struct pw_array servers;
};

static int module_native_protocol_tcp_load(struct client *client, struct module *module)
{
	struct module_native_protocol_tcp_data *data = module->user_data;
	struct impl *impl = client->impl;
	const char *address;
	int res;

	if ((address = pw_properties_get(module->props, "pulse.tcp")) == NULL)
		return -EIO;

	pw_array_init(&data->servers, sizeof(struct server *));

	res = servers_create_and_start(impl, address, &data->servers);
	if (res < 0)
		return res;

	return 0;
}

static int module_native_protocol_tcp_unload(struct module *module)
{
	struct module_native_protocol_tcp_data *d = module->user_data;
	struct server **s;

	pw_array_for_each (s, &d->servers)
		server_free(*s);

	pw_array_clear(&d->servers);

	return 0;
}

static const struct module_methods module_native_protocol_tcp_methods = {
	VERSION_MODULE_METHODS,
	.load = module_native_protocol_tcp_load,
	.unload = module_native_protocol_tcp_unload,
};

static const struct spa_dict_item module_native_protocol_tcp_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Native protocol (TCP sockets)" },
	{ PW_KEY_MODULE_USAGE, "port=<TCP port number> "
				"listen=<address to listen on>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_native_protocol_tcp(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_native_protocol_tcp_data *d;
	struct pw_properties *props = NULL;
	const char *port, *listen;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_native_protocol_tcp_info));
	if (props == NULL) {
		res = -errno;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if ((port = pw_properties_get(props, "port")) == NULL)
		port = SPA_STRINGIFY(PW_PROTOCOL_PULSE_DEFAULT_PORT);

	listen = pw_properties_get(props, "listen");

	pw_properties_setf(props, "pulse.tcp", "[ \"tcp:%s%s%s\" ]",
			   listen ? listen : "", listen ? ":" : "", port);

	module = module_new(impl, &module_native_protocol_tcp_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;

	return module;
out:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}
