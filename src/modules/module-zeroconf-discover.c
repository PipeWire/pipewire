/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>
#include <pipewire/private.h>
#include <pipewire/i18n.h>

#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include "module-zeroconf-discover/avahi-poll.h"

#define NAME "zeroconf-discover"

#define MODULE_USAGE	" "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Discover remote streams" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define SERVICE_TYPE_SINK "_pulse-sink._tcp"
#define SERVICE_TYPE_SOURCE "_non-monitor._sub._pulse-source._tcp"


struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_work_queue *work;

	struct pw_properties *properties;

	AvahiPoll *avahi_poll;
	AvahiClient *client;
	AvahiServiceBrowser *sink_browser;
	AvahiServiceBrowser *source_browser;

	struct spa_list tunnel_list;

	unsigned int unloading:1;
};

struct tunnel_info {
	AvahiIfIndex interface;
	AvahiProtocol protocol;
	const char *name;
	const char *type;
	const char *domain;
};

#define TUNNEL_INFO(...) (struct tunnel_info){ __VA_ARGS__ }

struct tunnel {
	struct spa_list link;
	struct tunnel_info info;
	struct pw_impl_module *module;
	struct spa_hook module_listener;
};

static int start_client(struct impl *impl);

static void do_unload_module(void *obj, void *data, int res, uint32_t id)
{
	struct impl *impl = data;
	pw_impl_module_destroy(impl->module);
}

static void unload_module(struct impl *impl)
{
	if (!impl->unloading) {
		impl->unloading = true;
		pw_work_queue_add(impl->work, impl, 0, do_unload_module, impl);
	}
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->module_listener);

	if (impl->properties)
		pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static struct tunnel *make_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;

	t->info.interface = info->interface;
	t->info.protocol = info->protocol;
	t->info.name = strdup(info->name);
	t->info.type = strdup(info->type);
	t->info.domain = strdup(info->domain);
	spa_list_append(&impl->tunnel_list, &t->link);

	return t;
}

static struct tunnel *find_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnel_list, link) {
		if (t->info.interface == info->interface &&
		    t->info.protocol == info->protocol &&
		    strcmp(t->info.name, info->name) == 0 &&
		    strcmp(t->info.type, info->type) == 0 &&
		    strcmp(t->info.domain, info->domain) == 0)
			return t;
	}
	return NULL;
}

static void free_tunnel(struct tunnel *t)
{
	spa_list_remove(&t->link);
	if (t->module)
		pw_impl_module_destroy(t->module);
	free((char*)t->info.name);
	free((char*)t->info.type);
	free((char*)t->info.domain);
	free(t);
}

static void serialize_dict(FILE *f, const struct spa_dict *dict)
{
	const struct spa_dict_item *it;
	spa_dict_for_each(it, dict) {
		size_t len = it->value ? strlen(it->value) : 0;
		fprintf(f, " \"%s\" = ", it->key);
		if (it->value == NULL) {
			fprintf(f, "null");
		} else if (spa_json_is_null(it->value, len) ||
		    spa_json_is_float(it->value, len) ||
		    spa_json_is_bool(it->value, len) ||
		    spa_json_is_container(it->value, len)) {
			fprintf(f, "%s", it->value);
		} else {
			size_t size = (len+1) * 4;
			char str[size];
			spa_json_encode_string(str, size, it->value);
			fprintf(f, "%s", str);
		}
	}
}

static void resolver_cb(AvahiServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiResolverEvent event, const char *name, const char *type, const char *domain,
	const char *host_name, const AvahiAddress *a, uint16_t port, AvahiStringList *txt,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel *t;
	struct tunnel_info tinfo;
	const char *str, *device;
	char if_suffix[16] = "";
	char at[AVAHI_ADDRESS_STR_MAX];
	AvahiStringList *l;
	FILE *f;
	char *args;
	size_t size;
	struct pw_impl_module *mod;
	struct pw_properties *props = NULL;

	if (event != AVAHI_RESOLVER_FOUND) {
		pw_log_error("Resolving of '%s' failed: %s", name,
				avahi_strerror(avahi_client_errno(impl->client)));
		return;
	}

	props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		pw_log_error("Can't allocate properties: %m");
		return;
	}

	tinfo = TUNNEL_INFO(.interface = interface,
			.protocol = protocol,
			.name = name,
			.type = type,
			.domain = domain);

	for (l = txt; l; l = l->next) {
		char *key, *value;

		if (avahi_string_list_get_pair(l, &key, &value, NULL) != 0)
			break;

		if (strcmp(key, "device") == 0) {
			pw_properties_set(props, "node.target", value);
		}
		else if (strcmp(key, "rate") == 0) {
			pw_properties_setf(props, "audio.rate", "%u", atoi(value));
		}
		else if (strcmp(key, "channels") == 0) {
			pw_properties_setf(props, "audio.channels", "%u", atoi(value));
		}
		else if (strcmp(key, "format") == 0) {
			pw_properties_set(props, "audio.format", value);
		}
		else if (strcmp(key, "icon-name") == 0) {
			pw_properties_set(props, "device.icon-name", value);
		}
		else if (strcmp(key, "channel_map") == 0) {
		}
		avahi_free(key);
		avahi_free(value);
	}

	if ((device = pw_properties_get(props, "node.target")) != NULL)
		pw_properties_setf(props, "node.name",
				"tunnel.%s.%s", host_name, device);
	else
		pw_properties_setf(props, "node.name",
				"tunnel.%s", host_name);


	str = strstr(type, "sink") ? "playback" : "capture";
	pw_properties_set(props, "tunnel.mode", str);

	if (a->proto == AVAHI_PROTO_INET6 &&
	    a->data.ipv6.address[0] == 0xfe &&
	    (a->data.ipv6.address[1] & 0xc0) == 0x80)
		snprintf(if_suffix, sizeof(if_suffix), "%%%d", interface);

	pw_properties_setf(props, "pulse.server.address", "[%s%s]:%u",
			avahi_address_snprint(at, sizeof(at), a),
			if_suffix, port);

	if ((str = pw_properties_get(props, "pulse.server.address")) != NULL)
		pw_properties_setf(props, "node.description",
				_("Tunnel to %s/%s"), str, device);

	f = open_memstream(&args, &size);
	fprintf(f, "{");
	serialize_dict(f, &props->dict);
	fprintf(f, " stream.props = {");
	fprintf(f, " }");
	fprintf(f, "}");
        fclose(f);

	pw_properties_free(props);

	mod = pw_context_load_module(impl->context,
			"libpipewire-module-pulse-tunnel",
			args, NULL);
	free(args);

	if (mod == NULL) {
		pw_log_error("Can't load module: %m");
                return;
	}

	t = make_tunnel(impl, &tinfo);
	if (t == NULL)
		return;

	t->module = mod;
}


static void browser_cb(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel_info info;
	struct tunnel *t;

	if (flags & AVAHI_LOOKUP_RESULT_LOCAL)
		return;

	info = TUNNEL_INFO(.interface = interface,
			.protocol = protocol,
			.name = name,
			.type = type,
			.domain = domain);

	t = find_tunnel(impl, &info);

	switch (event) {
	case AVAHI_BROWSER_NEW:
		if (t != NULL)
			return;
		if (!(avahi_service_resolver_new(impl->client,
						interface, protocol,
						name, type, domain,
						AVAHI_PROTO_UNSPEC, 0,
						resolver_cb, impl)))
			pw_log_error("can't make service resolver: %s",
					avahi_strerror(avahi_client_errno(impl->client)));
		break;
	case AVAHI_BROWSER_REMOVE:
		if (t == NULL)
			return;
		free_tunnel(t);
		break;
	default:
		break;
	}
}


static struct AvahiServiceBrowser *make_browser(struct impl *impl, const char *service_type)
{
	struct AvahiServiceBrowser *s;

	s = avahi_service_browser_new(impl->client,
                              AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                              service_type, NULL, 0,
                              browser_cb, impl);
	if (s == NULL) {
		pw_log_error("can't make browser for %s: %s", service_type,
				avahi_strerror(avahi_client_errno(impl->client)));
	}
	return s;
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata)
{
	struct impl *impl = userdata;

	impl->client = c;

	switch (state) {
	case AVAHI_CLIENT_S_REGISTERING:
	case AVAHI_CLIENT_S_RUNNING:
	case AVAHI_CLIENT_S_COLLISION:
		if (impl->sink_browser == NULL)
			impl->sink_browser = make_browser(impl, SERVICE_TYPE_SINK);
		if (impl->sink_browser == NULL)
			goto error;

		if (impl->source_browser == NULL)
			impl->source_browser = make_browser(impl, SERVICE_TYPE_SOURCE);
		if (impl->source_browser == NULL)
			goto error;

		break;
	case AVAHI_CLIENT_FAILURE:
		if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED)
			start_client(impl);

		SPA_FALLTHROUGH;
	case AVAHI_CLIENT_CONNECTING:
		if (impl->sink_browser) {
			avahi_service_browser_free(impl->sink_browser);
			impl->sink_browser = NULL;
		}
		if (impl->source_browser) {
			avahi_service_browser_free(impl->source_browser);
			impl->source_browser = NULL;
		}
		break;
	default:
		break;
	}
	return;
error:
	unload_module(impl);
}

static int start_client(struct impl *impl)
{
	int res;
	if ((impl->client = avahi_client_new(impl->avahi_poll,
					AVAHI_CLIENT_NO_FAIL,
					client_callback, impl,
					&res)) == NULL) {
		pw_log_error("can't create client: %s", avahi_strerror(res));
		unload_module(impl);
		return -EIO;
	}
	return 0;
}

static int start_avahi(struct impl *impl)
{
	struct pw_loop *loop;

	loop = pw_context_get_main_loop(impl->context);
	impl->avahi_poll = pw_avahi_poll_new(loop);

	return start_client(impl);;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL)
		goto error_errno;

	spa_list_init(&impl->tunnel_list);

	impl->module = module;
	impl->context = context;
	impl->work = pw_context_get_work_queue(context);
	impl->properties = props;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	start_avahi(impl);

	return 0;

error_errno:
	res = -errno;
	free(impl);
	return res;
}
