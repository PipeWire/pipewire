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
#include "zeroconf-utils/zeroconf.h"

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

	struct pw_zeroconf *zeroconf;
	struct spa_hook zeroconf_listener;

	struct spa_list tunnel_list;
};

struct tunnel_info {
	const char *name;
	const char *mode;
};

#define TUNNEL_INFO(...) ((struct tunnel_info){ __VA_ARGS__ })

struct tunnel {
	struct spa_list link;
	struct tunnel_info info;
	struct pw_impl_module *module;
	struct spa_hook module_listener;
};

static struct tunnel *tunnel_new(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;

	t->info.name = strdup(info->name);
	t->info.mode = strdup(info->mode);
	spa_list_append(&impl->tunnel_list, &t->link);

	return t;
}

static struct tunnel *find_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnel_list, link) {
		if (spa_streq(t->info.name, info->name) &&
		    spa_streq(t->info.mode, info->mode))
			return t;
	}
	return NULL;
}

static void tunnel_free(struct tunnel *t)
{
	spa_list_remove(&t->link);
	if (t->module)
		pw_impl_module_destroy(t->module);
	free((char *) t->info.name);
	free((char *) t->info.mode);

	free(t);
}

static void impl_free(struct impl *impl)
{
	struct tunnel *t;

	spa_list_consume(t, &impl->tunnel_list, link)
		tunnel_free(t);

	if (impl->zeroconf)
		pw_zeroconf_destroy(impl->zeroconf);
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

static void pw_properties_from_zeroconf(const char *key, const char *value,
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

static void on_zeroconf_added(void *data, void *user_data, const struct spa_dict *info)
{
	struct impl *impl = data;
	const char *name, *type, *mode, *device, *host_name, *desc, *fqdn, *user, *str;
	struct tunnel *t;
	struct tunnel_info tinfo;
	const struct spa_dict_item *it;
	FILE *f;
	char *args;
	size_t size;
	struct pw_impl_module *mod;
	struct pw_properties *props = NULL;

	name = spa_dict_lookup(info, "zeroconf.name");
	type = spa_dict_lookup(info, "zeroconf.type");
	mode = strstr(type, "sink") ? "sink" : "source";

	tinfo = TUNNEL_INFO(.name = name, .mode = mode);

	t = find_tunnel(impl, &tinfo);
	if (t == NULL)
		t = tunnel_new(impl, &tinfo);
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

	spa_dict_for_each(it, info)
		pw_properties_from_zeroconf(it->key, it->value, props);

	host_name = spa_dict_lookup(info, "zeroconf.hostname");

	if ((device = pw_properties_get(props, PW_KEY_TARGET_OBJECT)) != NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME,
				"tunnel.%s.%s", host_name, device);
	else
		pw_properties_setf(props, PW_KEY_NODE_NAME,
				"tunnel.%s", host_name);

	pw_properties_set(props, "tunnel.mode", mode);

	pw_properties_setf(props, "pulse.server.address", " [%s]:%s",
			spa_dict_lookup(info, "zeroconf.address"),
			spa_dict_lookup(info, "zeroconf.port"));

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
	pw_properties_free(props);
}

static void on_zeroconf_removed(void *data, void *user, const struct spa_dict *info)
{
	struct impl *impl = data;
	const char *name, *type, *mode;
	struct tunnel *t;
	struct tunnel_info tinfo;

	name = spa_dict_lookup(info, "zeroconf.name");
	type = spa_dict_lookup(info, "zeroconf.type");
	mode = strstr(type, "sink") ? "sink" : "source";

	tinfo = TUNNEL_INFO(.name = name, .mode = mode);

	if ((t = find_tunnel(impl, &tinfo)) == NULL)
		return;

	tunnel_free(t);
}

static const struct pw_zeroconf_events zeroconf_events = {
	PW_VERSION_ZEROCONF_EVENTS,
	.added = on_zeroconf_added,
	.removed = on_zeroconf_removed,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	bool discover_local;
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

	discover_local = pw_properties_get_bool(impl->properties,
			"pulse.discover-local", false);
	pw_properties_setf(impl->properties, "zeroconf.discover-local",
			discover_local ? "true" : "false");

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	if ((impl->zeroconf = pw_zeroconf_new(context, &props->dict)) == NULL) {
		pw_log_error("can't create zeroconf: %m");
		goto error_errno;
	}
	pw_zeroconf_add_listener(impl->zeroconf, &impl->zeroconf_listener,
			&zeroconf_events, impl);

	pw_zeroconf_set_browse(impl->zeroconf, SERVICE_TYPE_SINK,
		&SPA_DICT_ITEMS(
			SPA_DICT_ITEM("zeroconf.service", SERVICE_TYPE_SINK)));

	pw_zeroconf_set_browse(impl->zeroconf, SERVICE_TYPE_SOURCE,
		&SPA_DICT_ITEMS(
			SPA_DICT_ITEM("zeroconf.service", SERVICE_TYPE_SOURCE)));

	return 0;

error_errno:
	res = -errno;
	if (impl)
		impl_free(impl);
	return res;
}
