/* PipeWire
 *
 * Copyright © 2020 Georges Basile Stavracas Neto
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

struct module;

typedef void (*module_loaded_cb)(struct module *module, int error, void *userdata);

struct module_events {
	void (*removed) (void *data, struct module *module);
	void (*error) (void *data);
};

struct module {
	struct spa_list link;           /**< link in client modules */
	struct pw_proxy *proxy;
	struct spa_hook listener;
	struct client *client;
	struct message *reply;

	module_loaded_cb cb;
	void *cb_data;

	struct module_events *events;
	void *events_data;

	uint32_t idx;
};

static void module_proxy_removed(void *data)
{
	struct module *module = data;

	if (module->events)
		module->events->removed(module->events_data, module);

	pw_proxy_destroy(module->proxy);
}

static void module_proxy_destroy(void *data)
{
	struct module *module = data;
	pw_log_info(NAME" %p: proxy %p destroy", module, module->proxy);
	spa_hook_remove(&module->listener);
	free(module);
}

static void module_proxy_bound(void *data, uint32_t global_id)
{
	struct module *module = data;

	pw_log_info(NAME" module %p proxy %p bound", module, module->proxy);

	module->idx = global_id;

	if (module->cb)
		module->cb(module, 0, module->cb_data);
}

static void module_proxy_error(void *data, int seq, int res, const char *message)
{
	struct module *module = data;
	struct impl *impl = module->client->impl;

	pw_log_info(NAME" %p module %p error %d", impl, module, res);

	module->idx = 0;

	if (module->cb)
		module->cb(module, res, module->cb_data);

	pw_proxy_destroy(module->proxy);
}

static int load_null_sink_module(struct client *client, struct module *module, struct pw_properties *props)
{
	static const struct pw_proxy_events proxy_events = {
		.removed = module_proxy_removed,
		.bound = module_proxy_bound,
		.error = module_proxy_error,
		.destroy = module_proxy_destroy,
	};

	module->proxy = pw_core_create_object(client->core,
                                "adapter",
                                PW_TYPE_INTERFACE_Node,
                                PW_VERSION_NODE,
                                props ? &props->dict : NULL, 0);
	if (module->proxy == NULL)
		return -errno;

	pw_proxy_add_listener(module->proxy, &module->listener, &proxy_events, module);
	return 0;
}

static void add_props(struct pw_properties *props, const char *str)
{
	char *s = strdup(str), *p = s, *e, f;
	const char *k, *v;

	while (*p) {
		e = strchr(p, '=');
		if (e == NULL)
			break;
		*e = '\0';
		k = p;
		p = e+1;

		if (*p == '\"') {
			p++;
			f = '\"';
		} else {
			f = ' ';
		}
		e = strchr(p, f);
		if (e == NULL)
			e = strchr(p, '\0');
		if (e == NULL)
			break;
		*e = '\0';
		v = p;
		p = e + 1;
		pw_properties_set(props, k, v);
	}
	free(s);
}

static int load_module(struct client *client, const char *name, const char *argument, module_loaded_cb cb, void *data)
{
	struct module *module = NULL;
	int res = -ENOENT;

	if (strcmp(name, "module-null-sink") == 0) {
		struct pw_properties *props = NULL;
		const char *str;

		if (argument == NULL) {
			res = -EINVAL;
			goto out;
		}
		props = pw_properties_new(NULL, NULL);
		if (props == NULL) {
			res = -EINVAL;
			goto out;
		}
		add_props(props, argument);

		if ((str = pw_properties_get(props, "sink_name")) != NULL) {
			pw_properties_set(props, PW_KEY_NODE_NAME, str);
			pw_properties_set(props, "sink_name", NULL);
		} else {
			pw_properties_set(props, PW_KEY_NODE_NAME, "null");
		}
		if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
			add_props(props, str);
			pw_properties_set(props, "sink_properties", NULL);
		}
		if ((str = pw_properties_get(props, "channels")) != NULL) {
			pw_properties_set(props, SPA_KEY_AUDIO_CHANNELS, str);
			pw_properties_set(props, "channels", NULL);
		}
		if ((str = pw_properties_get(props, "rate")) != NULL) {
			pw_properties_set(props, SPA_KEY_AUDIO_RATE, str);
			pw_properties_set(props, "rate", NULL);
		}
		if ((str = pw_properties_get(props, "channel_map")) != NULL) {
			struct channel_map map = CHANNEL_MAP_INIT;
			uint32_t i;
			char *s, *p;

			channel_map_parse(str, &map);
			p = s = alloca(map.channels * 6);

			for (i = 0; i < map.channels; i++)
				p += snprintf(p, 6, "%s%s", i == 0 ? "" : ",",
						channel_id2name(map.map[i]));
			pw_properties_set(props, SPA_KEY_AUDIO_POSITION, s);
			pw_properties_set(props, "channel_map", NULL);
		}
		if ((str = pw_properties_get(props, "device.description")) != NULL) {
			pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, str);
			pw_properties_set(props, "device.description", NULL);
		}
		pw_properties_set(props, PW_KEY_FACTORY_NAME, "support.null-audio-sink");

		module = calloc(1, sizeof(struct module));
		module->client = client;
		module->cb = cb;
		module->cb_data = data;

		if ((res = load_null_sink_module(client, module, props)) < 0)
			goto out;
	}
out:
	if (res < 0) {
		free(module);
		module = NULL;
	}

	return res;
}

static void module_add_listener(struct module *module, struct module_events *events, void *userdata)
{
	module->events = events;
	module->events_data = userdata;
}

static void unload_module(struct module *module)
{
	pw_proxy_destroy(module->proxy);
}
