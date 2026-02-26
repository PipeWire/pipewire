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

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include "zeroconf-utils/zeroconf.h"
#include "module-protocol-pulse/format.h"

/** \page page_module_raop_discover RAOP Discover
 *
 * Automatically creates RAOP (Airplay) sink devices based on zeroconf
 * information.
 *
 * This module will load module-raop-sink for each announced stream that matches
 * the rule with the create-stream action.
 *
 * If no stream.rules are given, it will create a sink for all announced
 * streams.
 *
 * ## Module Name
 *
 * `libpipewire-module-raop-discover`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `raop.discover-local` = allow discovery of local services as well.
 *    false by default.
 * - `raop.latency.ms` = latency for all streams in microseconds. This
 *    can be overwritten in the stream rules.
 * - `stream.rules` = <rules>: match rules, use create-stream actions. See
 *   \ref page_module_raop_sink for module properties.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-raop-discover.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-raop-discover
 *     args = {
 *         #raop.discover-local = false;
 *         #raop.latency.ms = 1000
 *         stream.rules = [
 *             {   matches = [
 *                     {    raop.ip = "~.*"
 *                          #raop.port = 1000
 *                          #raop.name = ""
 *                          #raop.hostname = ""
 *                          #raop.domain = ""
 *                          #raop.device = ""
 *                          #raop.transport = "udp" | "tcp"
 *                          #raop.encryption.type = "none" | "RSA" | "auth_setup" | "fp_sap25"
 *                          #raop.audio.codec = "PCM" | "ALAC" | "AAC" | "AAC-ELD"
 *                          #audio.channels = 2
 *                          #audio.format = "S16" | "S24" | "S32"
 *                          #audio.rate = 44100
 *                          #device.model = ""
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         #raop.password = ""
 *                         stream.props = {
 *                             #target.object = ""
 *                             #media.class = "Audio/Sink"
 *                         }
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * ## See also
 *
 * \ref page_module_raop_sink
 */

#define NAME "raop-discover"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE "( stream.rules=<rules>, use create-stream actions )"

#define DEFAULT_CREATE_RULES	\
        "[ { matches = [ { raop.ip = \"~.*\" } ] actions = { create-stream = { } } } ] "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Discover remote streams" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define SERVICE_TYPE_SINK "_raop._tcp"

struct impl {
	struct pw_context *context;

	bool discover_local;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_properties *properties;

	struct pw_zeroconf *zeroconf;
	struct spa_hook zeroconf_listener;

	struct spa_list tunnel_list;
};

struct tunnel {
	struct spa_list link;
	char *name;
	struct pw_impl_module *module;
	struct spa_hook module_listener;
};

static struct tunnel *tunnel_new(struct impl *impl, const char *name)
{
	struct tunnel *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;

	t->name = strdup(name);
	spa_list_append(&impl->tunnel_list, &t->link);

	return t;
}

static struct tunnel *find_tunnel(struct impl *impl, const char *name)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnel_list, link) {
		if (spa_streq(t->name, name))
			return t;
	}
	return NULL;
}

static void tunnel_free(struct tunnel *t)
{
	spa_list_remove(&t->link);
	if (t->module)
		pw_impl_module_destroy(t->module);
	free(t->name);
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

static bool str_in_list(const char *haystack, const char *delimiters, const char *needle)
{
	const char *s, *state = NULL;
	size_t len;
	while ((s = pw_split_walk(haystack, delimiters, &len, &state))) {
		if (spa_strneq(needle, s, len))
	            return true;
	}
	return false;
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

struct match_info {
	struct impl *impl;
	struct pw_properties *props;
	struct tunnel *tunnel;
	bool matched;
};

static int create_stream(struct impl *impl, struct pw_properties *props,
		struct tunnel *t)
{
	FILE *f;
	char *args;
	size_t size;
	int res = 0;
	struct pw_impl_module *mod;

	if ((f = open_memstream(&args, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		goto done;
	}

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &props->dict, 0);
	fprintf(f, "}");
        fclose(f);

	pw_log_info("loading module args:'%s'", args);
	mod = pw_context_load_module(impl->context,
			"libpipewire-module-raop-sink",
			args, NULL);
	free(args);

	if (mod == NULL) {
		res = -errno;
		pw_log_error("Can't load module: %m");
                goto done;
	}

	pw_impl_module_add_listener(mod, &t->module_listener, &submodule_events, t);
	t->module = mod;
done:
	return res;
}

static int rule_matched(void *data, const char *location, const char *action,
                        const char *str, size_t len)
{
	struct match_info *i = data;
	int res = 0;

	i->matched = true;
	if (spa_streq(action, "create-stream")) {
		pw_properties_update_string(i->props, str, len);
		create_stream(i->impl, i->props, i->tunnel);
	}
	return res;
}

static void pw_properties_from_zeroconf(const char *key, const char *value,
		struct pw_properties *props)
{
	if (spa_streq(key, "zeroconf.ifindex")) {
		pw_properties_set(props, "raop.ifindex", value);
	}
	else if (spa_streq(key, "zeroconf.address")) {
		pw_properties_set(props, "raop.ip", value);
	}
	else if (spa_streq(key, "zeroconf.port")) {
		pw_properties_set(props, "raop.port", value);
	}
	else if (spa_streq(key, "zeroconf.name")) {
		pw_properties_set(props, "raop.name", value);
	}
	else if (spa_streq(key, "zeroconf.hostname")) {
		pw_properties_set(props, "raop.hostname", value);
	}
	else if (spa_streq(key, "zeroconf.domain")) {
		pw_properties_set(props, "raop.domain", value);
	}
	else if (spa_streq(key, "device")) {
		pw_properties_set(props, "raop.device", value);
	}
	else if (spa_streq(key, "tp")) {
		/* transport protocol, "UDP", "TCP", "UDP,TCP" */
		if (str_in_list(value, ",", "UDP"))
			value = "udp";
		else if (str_in_list(value, ",", "TCP"))
			value = "tcp";
		pw_properties_set(props, "raop.transport", value);
	} else if (spa_streq(key, "et")) {
		/* RAOP encryption types:
		 *  0 = none,
		 *  1 = RSA,
		 *  3 = FairPlay,
		 *  4 = MFiSAP (/auth-setup),
		 *  5 = FairPlay SAPv2.5 */
		if (str_in_list(value, ",", "5"))
			value = "fp_sap25";
		else if (str_in_list(value, ",", "4"))
			value = "auth_setup";
		else if (str_in_list(value, ",", "1"))
			value = "RSA";
		else
			value = "none";
		pw_properties_set(props, "raop.encryption.type", value);
	} else if (spa_streq(key, "cn")) {
		/* Supported audio codecs:
		 *  0 = PCM,
		 *  1 = ALAC,
		 *  2 = AAC,
		 *  3 = AAC ELD. */
		if (str_in_list(value, ",", "0"))
			value = "PCM";
		else if (str_in_list(value, ",", "1"))
			value = "ALAC";
		else if (str_in_list(value, ",", "2"))
			value = "AAC";
		else if (str_in_list(value, ",", "3"))
			value = "AAC-ELD";
		else
			value = "unknown";
		pw_properties_set(props, "raop.audio.codec", value);
	} else if (spa_streq(key, "ch")) {
		/* Number of channels */
		pw_properties_set(props, PW_KEY_AUDIO_CHANNELS, value);
	} else if (spa_streq(key, "ss")) {
		/* Sample size */
		if (spa_streq(value, "16"))
			value = "S16";
		else if (spa_streq(value, "24"))
			value = "S24";
		else if (spa_streq(value, "32"))
			value = "S32";
		else
			value = "UNKNOWN";
		pw_properties_set(props, PW_KEY_AUDIO_FORMAT, value);
	} else if (spa_streq(key, "sr")) {
		/* Sample rate */
		pw_properties_set(props, PW_KEY_AUDIO_RATE, value);
	} else if (spa_streq(key, "am")) {
		/* Device model */
		pw_properties_set(props, "device.model", value);
        }
}

static void on_zeroconf_added(void *data, void *user, const struct spa_dict *info)
{
	struct impl *impl = data;
	const char *name, *str;
	struct tunnel *t;
	const struct spa_dict_item *it;
	struct pw_properties *props = NULL;

	name = spa_dict_lookup(info, "zeroconf.name");

	t = find_tunnel(impl, name);
	if (t == NULL) {
		if ((t = tunnel_new(impl, name)) == NULL) {
			pw_log_error("Can't make tunnel: %m");
			goto done;
		}
	}
	if (t->module != NULL) {
		pw_log_info("found duplicate mdns entry for %s on IP %s - "
				"skipping tunnel creation", name,
				spa_dict_lookup(info, "zeroconf.address"));
		goto done;
	}

	if ((props = pw_properties_new(NULL, NULL)) == NULL) {
		pw_log_error("Can't allocate properties: %m");
		goto done;
	}

	spa_dict_for_each(it, info)
		pw_properties_from_zeroconf(it->key, it->value, props);

	if ((str = pw_properties_get(impl->properties, "raop.latency.ms")) != NULL)
		pw_properties_set(props, "raop.latency.ms", str);

	if ((str = pw_properties_get(impl->properties, "stream.rules")) == NULL)
		str = DEFAULT_CREATE_RULES;
	if (str != NULL) {
		struct match_info minfo = {
			.impl = impl,
			.props = props,
			.tunnel = t,
		};
		pw_conf_match_rules(str, strlen(str), NAME, &props->dict,
				rule_matched, &minfo);

		if (!minfo.matched)
			pw_log_info("unmatched service found %s", str);
	}
done:
	pw_properties_free(props);
}

static void on_zeroconf_removed(void *data, void *user, const struct spa_dict *info)
{
	struct impl *impl = data;
	const char *name;
	struct tunnel *t;

	name = spa_dict_lookup(info, "zeroconf.name");

	if ((t = find_tunnel(impl, name)) == NULL)
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
	const char *local;
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

	if ((local = pw_properties_get(impl->properties, "raop.discover-local")) == NULL)
		local = "false";
	pw_properties_set(impl->properties, "zeroconf.discover-local", local);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	if ((impl->zeroconf = pw_zeroconf_new(context, &props->dict)) == NULL) {
		pw_log_error("can't create zeroconf: %m");
		goto error_errno;
	}
	pw_zeroconf_add_listener(impl->zeroconf, &impl->zeroconf_listener,
			&zeroconf_events, impl);

	pw_zeroconf_set_browse(impl->zeroconf, NULL,
		&SPA_DICT_ITEMS(
			SPA_DICT_ITEM("zeroconf.service", SERVICE_TYPE_SINK)));
	return 0;

error_errno:
	res = -errno;
	if (impl)
		impl_free(impl);
	return res;
}
