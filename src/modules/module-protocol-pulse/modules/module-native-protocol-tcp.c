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

static const struct spa_dict_item module_native_protocol_tcp_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Native protocol (TCP sockets)" },
	{ PW_KEY_MODULE_USAGE, "port=<TCP port number> "
				"listen=<address to listen on> "
				"auth-anonymous=<don't check for cookies?>"},
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_native_protocol_tcp_prepare(struct module * const module)
{
	struct module_native_protocol_tcp_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	const char *port, *listen, *auth;
	FILE *f;
	char *args;
	size_t size;

	PW_LOG_TOPIC_INIT(mod_topic);

	if ((port = pw_properties_get(props, "port")) == NULL)
		port = SPA_STRINGIFY(PW_PROTOCOL_PULSE_DEFAULT_PORT);

	listen = pw_properties_get(props, "listen");

	auth = pw_properties_get(props, "auth-anonymous");

	f = open_memstream(&args, &size);
	if (f == NULL)
		return -errno;

	fprintf(f, "[ { ");
	fprintf(f, " \"address\": \"tcp:%s%s%s\" ",
			   listen ? listen : "", listen ? ":" : "", port);
	if (auth && module_args_parse_bool(auth))
		fprintf(f, " \"client.access\": \"unrestricted\" ");
	fprintf(f, "} ]");
	fclose(f);

	pw_properties_set(props, "pulse.tcp", args);
	free(args);

	d->module = module;

	return 0;
}

DEFINE_MODULE_INFO(module_native_protocol_tcp) = {
	.name = "module-native-protocol-tcp",
	.prepare = module_native_protocol_tcp_prepare,
	.load = module_native_protocol_tcp_load,
	.unload = module_native_protocol_tcp_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_native_protocol_tcp_info),
	.data_size = sizeof(struct module_native_protocol_tcp_data),
};
