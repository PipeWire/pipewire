/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format-utils.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include "module-protocol-pulse/format.h"
#include "module-zeroconf-discover/avahi-poll.h"

/** \page page_module_zeroconf_discover Zeroconf Discover
 *
 * Use zeroconf to detect and load module-pulse-tunnel with the right
 * parameters. This will automatically create sinks and sources to stream
 * audio to/from remote PulseAudio servers. It also works with
 * module-protocol-pulse.
 *
 * ## Module Name
 *
 * `libpipewire-module-zeroconf-discover`
 *
 * ## Module Options
 *
 * - `pulse.discover-local` = allow discovery of local services as well.
 *    false by default.
 * - `pulse.latency`: the latency to end-to-end latency in milliseconds to
 *                    maintain (Default 200ms).
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-zeroconf-discover.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-zeroconf-discover
 *     args = { }
 * }
 * ]
 *\endcode
 */

#define NAME "zeroconf-discover"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"( pulse.latency=<latency in msec, default 200> ) "

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

	struct pw_properties *properties;

	bool discover_local;
	AvahiPoll *avahi_poll;
	AvahiClient *client;
	AvahiServiceBrowser *sink_browser;
	AvahiServiceBrowser *source_browser;

	struct spa_list tunnel_list;
};

struct tunnel_info {
	const char *name;
};

#define TUNNEL_INFO(...) ((struct tunnel_info){ __VA_ARGS__ })

struct tunnel {
	struct spa_list link;
	struct tunnel_info info;
	struct pw_impl_module *module;
	struct spa_hook module_listener;
};

static int start_client(struct impl *impl);

static struct tunnel *make_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;

	t->info.name = strdup(info->name);
	spa_list_append(&impl->tunnel_list, &t->link);

	return t;
}

static struct tunnel *find_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnel_list, link) {
		if (spa_streq(t->info.name, info->name))
			return t;
	}
	return NULL;
}

static void free_tunnel(struct tunnel *t)
{
	spa_list_remove(&t->link);
	if (t->module)
		pw_impl_module_destroy(t->module);
	free((char *) t->info.name);

	free(t);
}

static void impl_free(struct impl *impl)
{
	struct tunnel *t;

	spa_list_consume(t, &impl->tunnel_list, link)
		free_tunnel(t);

	if (impl->sink_browser)
		avahi_service_browser_free(impl->sink_browser);
	if (impl->source_browser)
		avahi_service_browser_free(impl->source_browser);
	if (impl->client)
		avahi_client_free(impl->client);
	if (impl->avahi_poll)
		pw_avahi_poll_free(impl->avahi_poll);
	pw_properties_free(impl->properties);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void pw_properties_from_avahi_string(const char *key, const char *value,
		struct pw_properties *props)
{
	if (spa_streq(key, "device")) {
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, value);
	}
	else if (spa_streq(key, "rate")) {
		pw_properties_set(props, PW_KEY_AUDIO_RATE, value);
	}
	else if (spa_streq(key, "channels")) {
		pw_properties_set(props, PW_KEY_AUDIO_CHANNELS, value);
	}
	else if (spa_streq(key, "channel_map")) {
		struct channel_map channel_map;
		uint32_t i, pos[CHANNELS_MAX];
		char *p, *s, buf[8];

		spa_zero(channel_map);
		channel_map_parse(value, &channel_map);
		channel_map_to_positions(&channel_map, pos, CHANNELS_MAX);

		p = s = alloca(4 + channel_map.channels * 8);
		p += spa_scnprintf(p, 2, "[");
		for (i = 0; i < channel_map.channels; i++)
			p += spa_scnprintf(p, 8, "%s%s", i == 0 ? "" : ",",
				channel_id2name(pos[i], buf, sizeof(buf)));
		p += spa_scnprintf(p, 2, "]");
		pw_properties_set(props, SPA_KEY_AUDIO_POSITION, s);
	}
	else if (spa_streq(key, "format")) {
		uint32_t fmt = format_paname2id(value, strlen(value));
		if (fmt != SPA_AUDIO_FORMAT_UNKNOWN)
			pw_properties_set(props, PW_KEY_AUDIO_FORMAT, format_id2name(fmt));
	}
	else if (spa_streq(key, "icon-name")) {
		pw_properties_set(props, PW_KEY_DEVICE_ICON_NAME, value);
	}
	else if (spa_streq(key, "product-name")) {
		pw_properties_set(props, PW_KEY_DEVICE_PRODUCT_NAME, value);
	}
	else if (spa_streq(key, "description")) {
		pw_properties_set(props, "tunnel.remote.description", value);
	}
	else if (spa_streq(key, "fqdn")) {
		pw_properties_set(props, "tunnel.remote.fqdn", value);
	}
	else if (spa_streq(key, "user-name")) {
		pw_properties_set(props, "tunnel.remote.user", value);
	}
}

static void submodule_destroy(void *data)
{
	struct tunnel *t = data;

	spa_hook_remove(&t->module_listener);
	t->module = NULL;
}

static const struct pw_impl_module_events submodule_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = submodule_destroy,
};

static void resolver_cb(AvahiServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiResolverEvent event, const char *name, const char *type, const char *domain,
	const char *host_name, const AvahiAddress *a, uint16_t port, AvahiStringList *txt,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel *t;
	struct tunnel_info tinfo;
	const char *str, *device, *desc, *fqdn, *user;
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
		goto done;
	}

	tinfo = TUNNEL_INFO(.name = name);

	t = find_tunnel(impl, &tinfo);
	if (t == NULL)
		t = make_tunnel(impl, &tinfo);
	if (t == NULL) {
		pw_log_error("Can't make tunnel: %m");
		goto done;
	}
	if (t->module != NULL) {
		pw_log_info("found duplicate mdns entry - skipping tunnel creation");
		goto done;
	}

	props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		pw_log_error("Can't allocate properties: %m");
		goto done;
	}

	for (l = txt; l; l = l->next) {
		char *key, *value;

		if (avahi_string_list_get_pair(l, &key, &value, NULL) != 0)
			break;

		pw_properties_from_avahi_string(key, value, props);
		avahi_free(key);
		avahi_free(value);
	}

	if ((device = pw_properties_get(props, PW_KEY_TARGET_OBJECT)) != NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME,
				"tunnel.%s.%s", host_name, device);
	else
		pw_properties_setf(props, PW_KEY_NODE_NAME,
				"tunnel.%s", host_name);

	str = strstr(type, "sink") ? "sink" : "source";
	pw_properties_set(props, "tunnel.mode", str);

	if (a->proto == AVAHI_PROTO_INET6 &&
	    a->data.ipv6.address[0] == 0xfe &&
	    (a->data.ipv6.address[1] & 0xc0) == 0x80)
		snprintf(if_suffix, sizeof(if_suffix), "%%%d", interface);

	pw_properties_setf(props, "pulse.server.address", " [%s%s]:%u",
			avahi_address_snprint(at, sizeof(at), a),
			if_suffix, port);

	desc = pw_properties_get(props, "tunnel.remote.description");
	if (desc == NULL)
		desc = pw_properties_get(props, PW_KEY_DEVICE_PRODUCT_NAME);
	if (desc == NULL)
		desc = pw_properties_get(props, PW_KEY_TARGET_OBJECT);
	if (desc == NULL)
		desc = _("Unknown device");

	fqdn = pw_properties_get(props, "tunnel.remote.fqdn");
	if (fqdn == NULL)
		fqdn = pw_properties_get(props, "pulse.server.address");
	if (fqdn == NULL)
		fqdn = host_name;

	user = pw_properties_get(props, "tunnel.remote.user");

	if (desc != NULL && user != NULL && fqdn != NULL) {
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
				_("%s on %s@%s"), desc, user, fqdn);
	}
	else if (desc != NULL && fqdn != NULL) {
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
				_("%s on %s"), desc, fqdn);
	}

	if ((str = pw_properties_get(impl->properties, "pulse.latency")) != NULL)
		pw_properties_set(props, "pulse.latency", str);

	if ((f = open_memstream(&args, &size)) == NULL) {
		pw_log_error("Can't open memstream: %m");
		goto done;
	}

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &props->dict, 0);
	fprintf(f, " stream.props = {");
	fprintf(f, " }");
	fprintf(f, "}");
        fclose(f);

	pw_log_info("loading module args:'%s'", args);
	mod = pw_context_load_module(impl->context,
			"libpipewire-module-pulse-tunnel",
			args, NULL);
	free(args);

	if (mod == NULL) {
		pw_log_error("Can't load module: %m");
                goto done;
	}

	pw_impl_module_add_listener(mod, &t->module_listener, &submodule_events, t);

	t->module = mod;

done:
	avahi_service_resolver_free(r);
	pw_properties_free(props);
}


static void browser_cb(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel_info info;
	struct tunnel *t;

	if ((flags & AVAHI_LOOKUP_RESULT_LOCAL) && !impl->discover_local)
		return;

	info = TUNNEL_INFO(.name = name);

	t = find_tunnel(impl, &info);

	switch (event) {
	case AVAHI_BROWSER_NEW:
		if (t != NULL) {
			pw_log_info("found duplicate mdns entry - skipping tunnel creation");
			return;
		}
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
	pw_impl_module_schedule_destroy(impl->module);
}

static int start_client(struct impl *impl)
{
	int res;
	if ((impl->client = avahi_client_new(impl->avahi_poll,
					AVAHI_CLIENT_NO_FAIL,
					client_callback, impl,
					&res)) == NULL) {
		pw_log_error("can't create client: %s", avahi_strerror(res));
		pw_impl_module_schedule_destroy(impl->module);
		return -EIO;
	}
	return 0;
}

static int start_avahi(struct impl *impl)
{
	impl->avahi_poll = pw_avahi_poll_new(impl->context);

	return start_client(impl);
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL)
		goto error_errno;

	spa_list_init(&impl->tunnel_list);

	impl->module = module;
	impl->context = context;
	impl->properties = props;

	impl->discover_local =  pw_properties_get_bool(impl->properties,
			"pulse.discover-local", false);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	start_avahi(impl);

	return 0;

error_errno:
	res = -errno;
	if (impl)
		impl_free(impl);
	return res;
}
