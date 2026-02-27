/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include <sys/utsname.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <pipewire/pipewire.h>

#include "../collect.h"
#include "../defs.h"
#include "../manager.h"
#include "../module.h"
#include "../pulse-server.h"
#include "../server.h"
#include "../../zeroconf-utils/zeroconf.h"

/** \page page_pulse_module_zeroconf_publish Zeroconf Publish
 *
 * ## Module Name
 *
 * `module-zeroconf-publish`
 *
 * ## Module Options
 *
 * No options.
 */

#define NAME "zeroconf-publish"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define SERVICE_TYPE_SINK "_pulse-sink._tcp"
#define SERVICE_TYPE_SOURCE "_pulse-source._tcp"
#define SERVICE_TYPE_SERVER "_pulse-server._tcp"
#define SERVICE_SUBTYPE_SINK_HARDWARE "_hardware._sub."SERVICE_TYPE_SINK
#define SERVICE_SUBTYPE_SINK_VIRTUAL "_virtual._sub."SERVICE_TYPE_SINK
#define SERVICE_SUBTYPE_SOURCE_HARDWARE "_hardware._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_VIRTUAL "_virtual._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_MONITOR "_monitor._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_NON_MONITOR "_non-monitor._sub."SERVICE_TYPE_SOURCE

#define SERVICE_DATA_ID "module-zeroconf-publish.service"

struct service {
	struct spa_list link;

	struct module_zeroconf_publish_data *userdata;

	struct server *server;

	struct sample_spec ss;
	struct channel_map cm;
	struct pw_properties *props;

	unsigned published:1;
};

struct module_zeroconf_publish_data {
	struct module *module;

	struct pw_core *core;
	struct pw_manager *manager;

	struct spa_hook core_listener;
	struct spa_hook manager_listener;
	struct spa_hook impl_listener;

	struct pw_zeroconf *zeroconf;
	struct spa_hook zeroconf_listener;

	/* lists of services */
	struct spa_list pending;
	struct spa_list published;
};

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module_zeroconf_publish_data *d = data;
	struct module *module = d->module;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void unpublish_service(struct service *s)
{
	const char *device;

	spa_list_remove(&s->link);
	spa_list_append(&s->userdata->pending, &s->link);
	s->published = false;
	s->server = NULL;

	device = pw_properties_get(s->props, "device");

	pw_log_info("unpublished service: %s", device);

	pw_zeroconf_set_announce(s->userdata->zeroconf, s, NULL);
}

static void unpublish_all_services(struct module_zeroconf_publish_data *d)
{
	struct service *s;
	spa_list_consume(s, &d->published, link)
		unpublish_service(s);
}

static void service_free(struct service *s)
{
	pw_log_debug("service %p: free", s);

	if (s->published)
		unpublish_service(s);

	pw_properties_free(s->props);
	spa_list_remove(&s->link);
	/* no need to free, the service is added as custom
	 * data on the object */
}

#define PA_CHANNEL_MAP_SNPRINT_MAX (CHANNELS_MAX * 32)

static char* channel_map_snprint(char *s, size_t l, const struct channel_map *map)
{
	unsigned channel;
	bool first = true;
	char *e;
	uint32_t aux = 0;

	spa_assert(s);
	spa_assert(l > 0);
	spa_assert(map);

	if (!channel_map_valid(map)) {
		snprintf(s, l, "(invalid)");
		return s;
	}

	*(e = s) = 0;

	for (channel = 0; channel < map->channels && l > 1; channel++) {
		l -= spa_scnprintf(e, l, "%s%s",
				first ? "" : ",",
				channel_id2paname(map->map[channel], &aux));

		e = strchr(e, 0);
		first = false;
	}

	return s;
}

static void txt_record_server_data(struct pw_core_info *info, struct pw_properties *props)
{
	struct utsname u;

	spa_assert(info);

	pw_properties_set(props, "server-version", PACKAGE_NAME" "PACKAGE_VERSION);
	pw_properties_set(props, "user-name", pw_get_user_name());
	pw_properties_set(props, "fqdn", pw_get_host_name());
	pw_properties_setf(props, "cookie", "0x%08x", info->cookie);
	if (uname(&u) >= 0)
		pw_properties_setf(props, "uname", "%s %s %s", u.sysname, u.machine, u.release);
}

static void fill_service_txt(const struct service *s, const struct pw_properties *props)
{
	static const struct mapping {
		const char *pw_key, *txt_key;
	} mappings[] = {
		{ PW_KEY_NODE_DESCRIPTION, "description" },
		{ PW_KEY_DEVICE_VENDOR_NAME, "vendor-name" },
		{ PW_KEY_DEVICE_PRODUCT_NAME, "product-name" },
		{ PW_KEY_DEVICE_CLASS, "class" },
		{ PW_KEY_DEVICE_FORM_FACTOR, "form-factor" },
		{ PW_KEY_DEVICE_ICON_NAME, "icon-name" },
	};
	SPA_FOR_EACH_ELEMENT_VAR(mappings, m) {
		const char *value = pw_properties_get(props, m->pw_key);
		if (value != NULL)
			pw_properties_set(s->props, m->txt_key, value);
	}
}

static void fill_service_data(struct module_zeroconf_publish_data *d, struct service *s,
				struct pw_manager_object *o)
{
	bool is_sink = pw_manager_object_is_sink(o);
	bool is_source = pw_manager_object_is_source(o);
	struct pw_node_info *info = o->info;
	const char *name, *desc;
	struct pw_manager *manager = d->manager;
	struct pw_manager_object *card = NULL;
	struct card_info card_info = CARD_INFO_INIT;
	struct device_info dev_info;
	uint32_t flags = 0;
	const char *service_type, *subtype, *subtype_service[2];
	uint32_t n_subtype = 0;
	char cm[PA_CHANNEL_MAP_SNPRINT_MAX];


	if (info == NULL || info->props == NULL)
		return;

	name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
	if ((desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)) == NULL)
		desc = name ? name : "Unknown";
	if (name == NULL)
		name = "unknown";

	get_device_info(o, &dev_info, is_sink ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT, false);

	if (dev_info.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = dev_info.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	if (card)
		collect_card_info(card, &card_info);

	if (!pw_manager_object_is_virtual(o)) {
		if (is_sink)
			flags |= SINK_HARDWARE;
		else if (is_source)
			flags |= SOURCE_HARDWARE;
	}

	s->ss = dev_info.ss;
	s->cm = dev_info.map;

	s->props = pw_properties_new(NULL, NULL);

	txt_record_server_data(s->userdata->manager->info, s->props);

	if (is_sink) {
		service_type = SERVICE_TYPE_SINK;
		if (flags & SINK_HARDWARE) {
			subtype = "hardware";
			subtype_service[n_subtype++] = SERVICE_SUBTYPE_SINK_HARDWARE;
		} else {
			subtype = "virtual";
			subtype_service[n_subtype++] = SERVICE_SUBTYPE_SINK_VIRTUAL;
		}
	} else if (is_source) {
		service_type = SERVICE_TYPE_SOURCE;
		if (flags & SOURCE_HARDWARE) {
			subtype = "hardware";
			subtype_service[n_subtype++] = SERVICE_SUBTYPE_SOURCE_HARDWARE;
		} else {
			subtype = "virtual";
			subtype_service[n_subtype++] = SERVICE_SUBTYPE_SOURCE_VIRTUAL;
		}
		subtype_service[n_subtype++] = SERVICE_SUBTYPE_SOURCE_NON_MONITOR;
	} else
		spa_assert_not_reached();

	pw_properties_set(s->props, "device", name);
	pw_properties_setf(s->props, "rate", "%u", s->ss.rate);
	pw_properties_setf(s->props, "channels", "%u", s->ss.channels);
	pw_properties_set(s->props, "format", format_id2paname(s->ss.format));
	pw_properties_set(s->props, "channel_map", channel_map_snprint(cm, sizeof(cm), &s->cm));
	pw_properties_set(s->props, "subtype", subtype);

	pw_properties_setf(s->props, PW_KEY_ZEROCONF_NAME, "%s@%s: %s",
			pw_get_user_name(), pw_get_host_name(), desc);
	pw_properties_set(s->props, PW_KEY_ZEROCONF_TYPE, service_type);
	pw_properties_setf(s->props, PW_KEY_ZEROCONF_SUBTYPES, "[ %s%s%s ]",
			n_subtype > 0 ? subtype_service[0] : "",
			n_subtype > 1 ? ", " : "",
			n_subtype > 1 ? subtype_service[1] : "");

	fill_service_txt(s, o->props);
}

static struct service *create_service(struct module_zeroconf_publish_data *d, struct pw_manager_object *o)
{
	struct service *s;

	s = pw_manager_object_add_data(o, SERVICE_DATA_ID, sizeof(*s));
	if (s == NULL)
		return NULL;

	s->userdata = d;
	spa_list_append(&d->pending, &s->link);

	fill_service_data(d, s, o);

	pw_log_debug("service %p: created for object %p", s, o);

	return s;
}

static struct server *find_server(struct service *s, int *proto, uint16_t *port)
{
	struct module_zeroconf_publish_data *d = s->userdata;
	struct impl *impl = d->module->impl;
	struct server *server;

	spa_list_for_each(server, &impl->servers, link) {
		if (server->addr.ss_family == AF_INET) {
			*proto = 4;
			*port = ntohs(((struct sockaddr_in*) &server->addr)->sin_port);
			return server;
		} else if (server->addr.ss_family == AF_INET6) {
			*proto = 6;
			*port = ntohs(((struct sockaddr_in6*) &server->addr)->sin6_port);
			return server;
		}
	}
	return NULL;
}

static void publish_service(struct service *s)
{
	struct module_zeroconf_publish_data *d = s->userdata;
	int proto, res;
	uint16_t port;
	struct server *server = find_server(s, &proto, &port);
	const char *device;

	if (!server)
		return;

	device = pw_properties_get(s->props, "device");

	pw_log_debug("found server:%p proto:%d port:%d", server, proto, port);

	pw_properties_setf(s->props, PW_KEY_ZEROCONF_PROTO, "%d", proto);
	pw_properties_setf(s->props, PW_KEY_ZEROCONF_PORT, "%d", port);

	if ((res = pw_zeroconf_set_announce(s->userdata->zeroconf, s, &s->props->dict)) < 0) {
		pw_log_error("failed to announce service %s: %s", device, spa_strerror(res));
		return;
	}

	spa_list_remove(&s->link);
	spa_list_append(&d->published, &s->link);
	s->published = true;
	s->server = server;

	pw_log_info("published service: %s", device);
	return;
}

static void publish_pending(struct module_zeroconf_publish_data *data)
{
	struct service *s, *next;

	spa_list_for_each_safe(s, next, &data->pending, link)
		publish_service(s);
}

static void manager_removed(void *d, struct pw_manager_object *o)
{
	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source(o))
		return;

	struct service *s = pw_manager_object_get_data(o, SERVICE_DATA_ID);
	if (s == NULL)
		return;

	service_free(s);
}

static void manager_added(void *d, struct pw_manager_object *o)
{
	struct service *s;
	struct pw_node_info *info;
	struct module_zeroconf_publish_data *data = d;
	struct impl *impl = data->module->impl;

	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source(o))
		return;

	info = o->info;
	if (info == NULL || info->props == NULL)
		return;

	if (pw_manager_object_is_network(o))
		return;

	update_object_info(data->manager, o, &impl->defs);

	s = create_service(data, o);
	if (s == NULL)
		return;

	publish_service(s);
}

static const struct pw_manager_events manager_events = {
	PW_VERSION_MANAGER_EVENTS,
	.added = manager_added,
	.removed = manager_removed,
};


static void impl_server_started(void *data, struct server *server)
{
	struct module_zeroconf_publish_data *d = data;
	pw_log_info("a new server is started, try publish");
	publish_pending(d);
}

static void impl_server_stopped(void *data, struct server *server)
{
	struct module_zeroconf_publish_data *d = data;
	pw_log_info("a server stopped, try republish");

	struct service *s, *tmp;
	spa_list_for_each_safe(s, tmp, &d->published, link) {
		if (s->server == server)
			unpublish_service(s);
	}
	publish_pending(d);
}

static const struct impl_events impl_events = {
	VERSION_IMPL_EVENTS,
	.server_started = impl_server_started,
	.server_stopped = impl_server_stopped,
};

static void on_zeroconf_error(void *data, int err, const char *message)
{
	pw_log_error("got zeroconf error %d: %s", err, message);
}
static const struct pw_zeroconf_events zeroconf_events = {
	PW_VERSION_ZEROCONF_EVENTS,
	.error = on_zeroconf_error,
};

static int module_zeroconf_publish_load(struct module *module)
{
	struct module_zeroconf_publish_data *data = module->user_data;

	data->core = pw_context_connect(module->impl->context, NULL, 0);
	if (data->core == NULL) {
		pw_log_error("failed to connect to pipewire: %m");
		return -errno;
	}

	pw_core_add_listener(data->core,
			&data->core_listener,
			&core_events, data);

	data->manager = pw_manager_new(data->core);
	if (data->manager == NULL) {
		pw_log_error("failed to create pipewire manager: %m");
		return -errno;
	}
	pw_manager_add_listener(data->manager, &data->manager_listener,
			&manager_events, data);

	data->zeroconf = pw_zeroconf_new(module->impl->context, NULL);
	if (!data->zeroconf) {
		pw_log_error("failed to create zeroconf: %m");
		return -errno;
	}
	pw_zeroconf_add_listener(data->zeroconf, &data->zeroconf_listener,
			&zeroconf_events, data);

	impl_add_listener(module->impl, &data->impl_listener, &impl_events, data);

	return 0;
}

static int module_zeroconf_publish_unload(struct module *module)
{
	struct module_zeroconf_publish_data *d = module->user_data;
	struct service *s;

	spa_hook_remove(&d->impl_listener);

	unpublish_all_services(d);

	spa_list_consume(s, &d->pending, link)
		service_free(s);

	if (d->zeroconf) {
		spa_hook_remove(&d->zeroconf_listener);
		pw_zeroconf_destroy(d->zeroconf);
	}
	if (d->manager != NULL) {
		spa_hook_remove(&d->manager_listener);
		pw_manager_destroy(d->manager);
	}
	if (d->core != NULL) {
		spa_hook_remove(&d->core_listener);
		pw_core_disconnect(d->core);
	}
	return 0;
}

static const struct spa_dict_item module_zeroconf_publish_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io" },
	{ PW_KEY_MODULE_DESCRIPTION, "mDNS/DNS-SD Service Publish" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_zeroconf_publish_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct module_zeroconf_publish_data * const data = module->user_data;
	data->module = module;
	spa_list_init(&data->pending);
	spa_list_init(&data->published);

	return 0;
}

DEFINE_MODULE_INFO(module_zeroconf_publish) = {
	.name = "module-zeroconf-publish",
	.prepare = module_zeroconf_publish_prepare,
	.load = module_zeroconf_publish_load,
	.unload = module_zeroconf_publish_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_zeroconf_publish_info),
	.data_size = sizeof(struct module_zeroconf_publish_data),
};
