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
#include "../../module-zeroconf-discover/avahi-poll.h"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/domain.h>
#include <avahi-common/malloc.h>

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

enum service_subtype {
	SUBTYPE_HARDWARE,
	SUBTYPE_VIRTUAL,
	SUBTYPE_MONITOR
};

struct service {
	struct spa_list link;

	struct module_zeroconf_publish_data *userdata;

	AvahiEntryGroup *entry_group;
	AvahiStringList *txt;
	struct server *server;

	const char *service_type;
	enum service_subtype subtype;

	char *name;
	bool is_sink;

	struct sample_spec ss;
	struct channel_map cm;
	struct pw_properties *props;

	char service_name[AVAHI_LABEL_MAX];
	unsigned published:1;
};

struct module_zeroconf_publish_data {
	struct module *module;

	struct pw_core *core;
	struct pw_manager *manager;

	struct spa_hook core_listener;
	struct spa_hook manager_listener;
	struct spa_hook impl_listener;

	AvahiPoll *avahi_poll;
	AvahiClient *client;

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

static void get_service_name(struct pw_manager_object *o, char *buf, size_t length)
{
	const char *hn, *un, *n;

	hn = pw_get_host_name();
	un = pw_get_user_name();
	n = pw_properties_get(o->props, PW_KEY_NODE_DESCRIPTION);

	snprintf(buf, length, "%s@%s: %s", un, hn, n);
}

static void service_free(struct service *s)
{
	pw_log_debug("service %p: free", s);

	if (s->entry_group)
		avahi_entry_group_free(s->entry_group);

	if (s->name)
		free(s->name);

	pw_properties_free(s->props);
	avahi_string_list_free(s->txt);
	spa_list_remove(&s->link);
}

static void unpublish_service(struct service *s)
{
	spa_list_remove(&s->link);
	spa_list_append(&s->userdata->pending, &s->link);
	s->published = false;
	s->server = NULL;
}

static void unpublish_all_services(struct module_zeroconf_publish_data *d)
{
	struct service *s;

	spa_list_consume(s, &d->published, link)
		unpublish_service(s);
}

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
	s->name = strdup(name);
	s->props = pw_properties_copy(o->props);

	if (is_sink) {
		s->is_sink = true;
		s->service_type = SERVICE_TYPE_SINK;
		s->subtype = flags & SINK_HARDWARE ? SUBTYPE_HARDWARE : SUBTYPE_VIRTUAL;
	} else if (is_source) {
		s->is_sink = false;
		s->service_type = SERVICE_TYPE_SOURCE;
		s->subtype = flags & SOURCE_HARDWARE ? SUBTYPE_HARDWARE : SUBTYPE_VIRTUAL;
	} else
		spa_assert_not_reached();
}

static struct service *create_service(struct module_zeroconf_publish_data *d, struct pw_manager_object *o)
{
	struct service *s;

	s = pw_manager_object_add_data(o, SERVICE_DATA_ID, sizeof(*s));
	if (s == NULL)
		return NULL;

	s->userdata = d;
	s->entry_group = NULL;
	get_service_name(o, s->service_name, sizeof(s->service_name));
	spa_list_append(&d->pending, &s->link);

	fill_service_data(d, s, o);

	pw_log_debug("service %p: created for object %p", s, o);

	return s;
}

static AvahiStringList* txt_record_server_data(struct pw_core_info *info, AvahiStringList *l)
{
	const char *t;
	struct utsname u;

	spa_assert(info);

	l = avahi_string_list_add_pair(l, "server-version", PACKAGE_NAME" "PACKAGE_VERSION);

	t = pw_get_user_name();
	l = avahi_string_list_add_pair(l, "user-name", t);

	if (uname(&u) >= 0) {
		char sysname[sizeof(u.sysname) + sizeof(u.machine) + sizeof(u.release)];

		snprintf(sysname, sizeof(sysname), "%s %s %s", u.sysname, u.machine, u.release);
		l = avahi_string_list_add_pair(l, "uname", sysname);
	}

	t = pw_get_host_name();
	l = avahi_string_list_add_pair(l, "fqdn", t);
	l = avahi_string_list_add_printf(l, "cookie=0x%08x", info->cookie);

	return l;
}

static void clear_entry_group(struct service *s)
{
	if (s->entry_group == NULL)
		return;

	avahi_entry_group_free(s->entry_group);
	s->entry_group = NULL;
}

static void publish_service(struct service *s);

static void service_entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata)
{
	struct service *s = userdata;

	spa_assert(s);
	if (!s->published) {
		pw_log_info("cancel unpublished service: %s", s->service_name);
		clear_entry_group(s);
		return;
	}

	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		pw_log_info("established service: %s", s->service_name);
		break;
	case AVAHI_ENTRY_GROUP_COLLISION:
	{
		char *t;

		t = avahi_alternative_service_name(s->service_name);
		pw_log_info("service name collision: renaming '%s' to '%s'", s->service_name, t);
		snprintf(s->service_name, sizeof(s->service_name), "%s", t);
		avahi_free(t);

		unpublish_service(s);
		publish_service(s);
		break;
	}
	case AVAHI_ENTRY_GROUP_FAILURE:
		pw_log_error("failed to establish service '%s': %s",
			     s->service_name,
			     avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
		unpublish_service(s);
		clear_entry_group(s);
		break;

	case AVAHI_ENTRY_GROUP_UNCOMMITED:
	case AVAHI_ENTRY_GROUP_REGISTERING:
		break;
	}
}

#define PA_CHANNEL_MAP_SNPRINT_MAX (CHANNELS_MAX * 32)

static AvahiStringList *get_service_txt(const struct service *s)
{
	static const char * const subtype_text[] = {
		[SUBTYPE_HARDWARE] = "hardware",
		[SUBTYPE_VIRTUAL] = "virtual",
		[SUBTYPE_MONITOR] = "monitor"
	};

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

	char cm[PA_CHANNEL_MAP_SNPRINT_MAX];
	AvahiStringList *txt = NULL;

	txt = txt_record_server_data(s->userdata->manager->info, txt);

	txt = avahi_string_list_add_pair(txt, "device", s->name);
	txt = avahi_string_list_add_printf(txt, "rate=%u", s->ss.rate);
	txt = avahi_string_list_add_printf(txt, "channels=%u", s->ss.channels);
	txt = avahi_string_list_add_pair(txt, "format", format_id2paname(s->ss.format));
	txt = avahi_string_list_add_pair(txt, "channel_map", channel_map_snprint(cm, sizeof(cm), &s->cm));
	txt = avahi_string_list_add_pair(txt, "subtype", subtype_text[s->subtype]);

	SPA_FOR_EACH_ELEMENT_VAR(mappings, m) {
		const char *value = pw_properties_get(s->props, m->pw_key);
		if (value != NULL)
			txt = avahi_string_list_add_pair(txt, m->txt_key, value);
	}

	return txt;
}

static struct server *find_server(struct service *s, int *proto, uint16_t *port)
{
	struct module_zeroconf_publish_data *d = s->userdata;
	struct impl *impl = d->module->impl;
	struct server *server;

	spa_list_for_each(server, &impl->servers, link) {
		if (server->addr.ss_family == AF_INET) {
			*proto = AVAHI_PROTO_INET;
			*port = ntohs(((struct sockaddr_in*) &server->addr)->sin_port);
			return server;
		} else if (server->addr.ss_family == AF_INET6) {
			*proto = AVAHI_PROTO_INET6;
			*port = ntohs(((struct sockaddr_in6*) &server->addr)->sin6_port);
			return server;
		}
	}

	return NULL;
}

static void publish_service(struct service *s)
{
	struct module_zeroconf_publish_data *d = s->userdata;
	int proto;
	uint16_t port;

	struct server *server = find_server(s, &proto, &port);
	if (!server)
		return;

	pw_log_debug("found server:%p proto:%d port:%d", server, proto, port);

	if (!d->client || avahi_client_get_state(d->client) != AVAHI_CLIENT_S_RUNNING)
		return;

	s->published = true;
	if (!s->entry_group) {
		s->entry_group = avahi_entry_group_new(d->client, service_entry_group_callback, s);
		if (s->entry_group == NULL) {
			pw_log_error("avahi_entry_group_new(): %s",
					avahi_strerror(avahi_client_errno(d->client)));
			goto error;
		}
	} else {
		avahi_entry_group_reset(s->entry_group);
	}

	if (s->txt == NULL)
		s->txt = get_service_txt(s);

	if (avahi_entry_group_add_service_strlst(
				s->entry_group,
				AVAHI_IF_UNSPEC, proto,
				0,
				s->service_name,
				s->service_type,
				NULL,
				NULL,
				port,
				s->txt) < 0) {
		pw_log_error("avahi_entry_group_add_service_strlst(): %s",
			avahi_strerror(avahi_client_errno(d->client)));
		goto error;
	}

	if (avahi_entry_group_add_service_subtype(
				s->entry_group,
				AVAHI_IF_UNSPEC, proto,
				0,
				s->service_name,
				s->service_type,
				NULL,
				s->is_sink ? (s->subtype == SUBTYPE_HARDWARE ? SERVICE_SUBTYPE_SINK_HARDWARE : SERVICE_SUBTYPE_SINK_VIRTUAL) :
				(s->subtype == SUBTYPE_HARDWARE ? SERVICE_SUBTYPE_SOURCE_HARDWARE : (s->subtype == SUBTYPE_VIRTUAL ? SERVICE_SUBTYPE_SOURCE_VIRTUAL : SERVICE_SUBTYPE_SOURCE_MONITOR))) < 0) {

		pw_log_error("avahi_entry_group_add_service_subtype(): %s",
			avahi_strerror(avahi_client_errno(d->client)));
		goto error;
	}

	if (!s->is_sink && s->subtype != SUBTYPE_MONITOR) {
		if (avahi_entry_group_add_service_subtype(
					s->entry_group,
					AVAHI_IF_UNSPEC, proto,
					0,
					s->service_name,
					SERVICE_TYPE_SOURCE,
					NULL,
					SERVICE_SUBTYPE_SOURCE_NON_MONITOR) < 0) {
			pw_log_error("avahi_entry_group_add_service_subtype(): %s",
					avahi_strerror(avahi_client_errno(d->client)));
			goto error;
		}
	}

	if (avahi_entry_group_commit(s->entry_group) < 0) {
		pw_log_error("avahi_entry_group_commit(): %s",
				avahi_strerror(avahi_client_errno(d->client)));
		goto error;
	}

	spa_list_remove(&s->link);
	spa_list_append(&d->published, &s->link);
	s->server = server;

	pw_log_info("created service: %s", s->service_name);
	return;

error:
	s->published = false;
	return;
}

static void publish_pending(struct module_zeroconf_publish_data *data)
{
	struct service *s, *next;

	spa_list_for_each_safe(s, next, &data->pending, link)
		publish_service(s);
}

static void clear_pending_entry_groups(struct module_zeroconf_publish_data *data)
{
	struct service *s;

	spa_list_for_each(s, &data->pending, link)
		clear_entry_group(s);
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *d)
{
	struct module_zeroconf_publish_data *data = d;

	spa_assert(c);
	spa_assert(data);

	data->client = c;

	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		pw_log_info("the avahi daemon is up and running");
		publish_pending(data);
		break;
	case AVAHI_CLIENT_S_COLLISION:
		pw_log_error("host name collision");
		unpublish_all_services(d);
		break;
	case AVAHI_CLIENT_FAILURE:
	{
		int err = avahi_client_errno(data->client);

		pw_log_error("avahi client failure: %s", avahi_strerror(err));

		unpublish_all_services(data);
		clear_pending_entry_groups(data);
		avahi_client_free(data->client);
		data->client = NULL;

		if (err == AVAHI_ERR_DISCONNECTED) {
			data->client = avahi_client_new(data->avahi_poll, AVAHI_CLIENT_NO_FAIL, client_callback, data, &err);
			if (data->client == NULL)
				pw_log_error("failed to create avahi client: %s", avahi_strerror(err));
		}

		if (data->client == NULL)
			module_schedule_unload(data->module);

		break;
	}
	case AVAHI_CLIENT_CONNECTING:
		pw_log_info("connecting to the avahi daemon...");
		break;
	default:
		break;
	}
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

	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source(o))
		return;

	info = o->info;
	if (info == NULL || info->props == NULL)
		return;

	if (pw_manager_object_is_network(o))
		return;

	s = create_service(d, o);
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

static int module_zeroconf_publish_load(struct module *module)
{
	struct module_zeroconf_publish_data *data = module->user_data;
	struct pw_loop *loop;
	int error;

	data->core = pw_context_connect(module->impl->context, NULL, 0);
	if (data->core == NULL) {
		pw_log_error("failed to connect to pipewire: %m");
		return -errno;
	}

	pw_core_add_listener(data->core,
			&data->core_listener,
			&core_events, data);

	loop = pw_context_get_main_loop(module->impl->context);
	data->avahi_poll = pw_avahi_poll_new(loop);

	data->client = avahi_client_new(data->avahi_poll, AVAHI_CLIENT_NO_FAIL,
			client_callback, data, &error);
	if (!data->client) {
		pw_log_error("failed to create avahi client: %s", avahi_strerror(error));
		return -errno;
	}

	data->manager = pw_manager_new(data->core);
	if (data->manager == NULL) {
		pw_log_error("failed to create pipewire manager: %m");
		return -errno;
	}

	pw_manager_add_listener(data->manager, &data->manager_listener,
			&manager_events, data);

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

	if (d->client)
		avahi_client_free(d->client);

	if (d->avahi_poll)
		pw_avahi_poll_free(d->avahi_poll);

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
