/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io>
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

#include <sys/utsname.h>

#include <pipewire/pipewire.h>

#include "../collect.h"
#include "../defs.h"
#include "../manager.h"
#include "../module.h"
#include "../pulse-server.h"
#include "registry.h"
#include "../../module-zeroconf-discover/avahi-poll.h"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/domain.h>

#define SERVICE_TYPE_SINK "_pulse-sink._tcp"
#define SERVICE_TYPE_SOURCE "_pulse-source._tcp"
#define SERVICE_TYPE_SERVER "_pulse-server._tcp"
#define SERVICE_SUBTYPE_SINK_HARDWARE "_hardware._sub."SERVICE_TYPE_SINK
#define SERVICE_SUBTYPE_SINK_VIRTUAL "_virtual._sub."SERVICE_TYPE_SINK
#define SERVICE_SUBTYPE_SOURCE_HARDWARE "_hardware._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_VIRTUAL "_virtual._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_MONITOR "_monitor._sub."SERVICE_TYPE_SOURCE
#define SERVICE_SUBTYPE_SOURCE_NON_MONITOR "_non-monitor._sub."SERVICE_TYPE_SOURCE

enum service_subtype {
	SUBTYPE_HARDWARE,
	SUBTYPE_VIRTUAL,
	SUBTYPE_MONITOR
};

struct service {
	void *key;

	struct module_zeroconf_publish_data *userdata;
	AvahiEntryGroup *entry_group;
	char *service_name;
	const char *service_type;
	enum service_subtype subtype;

	char *name;
	bool is_sink;

	struct sample_spec ss;
	struct channel_map cm;
	struct pw_properties *props;
};

struct module_zeroconf_publish_data {
	struct module *module;

	struct pw_core *core;
	struct pw_manager *manager;

	struct spa_hook core_listener;
	struct spa_hook manager_listener;

	unsigned int port;

	AvahiPoll *avahi_poll;
	AvahiClient *client;
	bool entry_group_free;
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

static char *get_service_name(struct pw_manager_object *o)
{
	const char *hn, *un, *n;
	char *service_name;

	hn = pw_get_host_name();
	un = pw_get_user_name();
	n = pw_properties_get(o->props, PW_KEY_NODE_DESCRIPTION);

	service_name = calloc(1, AVAHI_LABEL_MAX - 1);
	snprintf(service_name, AVAHI_LABEL_MAX - 1, "%s@%s: %s", un, hn, n);

	return service_name;
}

static int service_free(void *d, struct pw_manager_object *o) {
	struct service *s;
	char *service_name;

	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source(o))
		return 0;

	service_name = get_service_name(o);

	s = pw_manager_object_add_data(o, service_name, sizeof(struct service));
	if (s == NULL) {
		pw_log_error("Could not find service %s to remove", service_name);
		free(service_name);
		return 0;
	}
	free(service_name);
	spa_assert(s);

	if (s->entry_group) {
		pw_log_debug("Removing entry group for %s.", s->service_name);
		avahi_entry_group_free(s->entry_group);
	}

	if (s->service_name) {
		pw_log_debug("Removing service: %s", s->service_name);
		free(s->service_name);
	}

	if (s->name) {
		pw_log_debug("Removing service for node: %s", s->name);
		free(s->name);
	}

	if (s->props)
		pw_properties_free(s->props);

	return 0;
}

static int unpublish_service(void *data, struct pw_manager_object *o) {
	struct module_zeroconf_publish_data *d = data;
	struct service *s;
	char *service_name;

	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source(o))
		return 0;

	service_name = get_service_name(o);

	s = pw_manager_object_add_data(o, service_name, sizeof(struct service));
	if (s == NULL) {
		pw_log_error("Could not find service %s to remove", service_name);
		free(service_name);
		return 0;
	}
	free(service_name);
	spa_assert(s);

	if (s->entry_group) {
		if (d->entry_group_free) {
			pw_log_debug("Removing entry group for %s.", s->service_name);
			avahi_entry_group_free(s->entry_group);
			s->entry_group = NULL;
		} else {
			avahi_entry_group_reset(s->entry_group);
			pw_log_debug("Resetting entry group for %s.", s->service_name);
		}
	}

	return 0;
}

static void unpublish_all_services(struct module_zeroconf_publish_data *d, bool entry_group_free)
{
	d->entry_group_free = entry_group_free;
	pw_manager_for_each_object(d->manager, unpublish_service, d);
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
		l -= snprintf(e, l, "%s%s",
				first ? "" : ",",
				channel_id2paname(map->map[channel], &aux));

		e = strchr(e, 0);
		first = false;
	}

	return s;
}

static void get_service_data(struct module_zeroconf_publish_data *d,
		struct service *s, struct pw_manager_object *o)
{
	struct impl *impl = d->module->impl;
	bool is_sink = pw_manager_object_is_sink(o);
	bool is_source = pw_manager_object_is_source(o);
	struct pw_node_info *info = o->info;
	const char *name, *desc, *str;
	uint32_t card_id = SPA_ID_INVALID;
	struct pw_manager *manager = d->manager;
	struct pw_manager_object *card = NULL;
	struct card_info card_info = CARD_INFO_INIT;
	struct device_info dev_info = is_sink ?
		DEVICE_INFO_INIT(PW_DIRECTION_OUTPUT) : DEVICE_INFO_INIT(PW_DIRECTION_INPUT);
	uint32_t flags = 0;

	if (info == NULL || info->props == NULL)
		return;

	name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
	if ((desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)) == NULL)
		desc = name ? name : "Unknown";
	if (name == NULL)
		name = "unknown";

	if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)) != NULL)
		card_id = (uint32_t)atoi(str);
	if ((str = spa_dict_lookup(info->props, "card.profile.device")) != NULL)
		dev_info.device = (uint32_t)atoi(str);
	if (card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	if (card)
		collect_card_info(card, &card_info);

	collect_device_info(o, card, &dev_info, false, &impl->defs);

	if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_API)) != NULL) {
		if (pw_manager_object_is_sink(o))
			flags |= SINK_HARDWARE;
		else if (pw_manager_object_is_source(o))
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

static struct service *get_service(struct module_zeroconf_publish_data *d, struct pw_manager_object *o)
{
	struct service *s;
	char *service_name;

	service_name = get_service_name(o);

	s = pw_manager_object_add_data(o, service_name, sizeof(struct service));
	spa_assert(s);

	s->key = o;
	s->userdata = d;
	s->entry_group = NULL;
	s->service_name = service_name;

	get_service_data(d, s, o);

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

static void publish_service(struct service *s);

static void service_entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata)
{
	struct service *s = userdata;

	spa_assert(s);

	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		pw_log_info("Successfully established service %s.", s->service_name);
		break;

	case AVAHI_ENTRY_GROUP_COLLISION:
	{
		char *t;

		t = avahi_alternative_service_name(s->service_name);
		pw_log_info("Name collision, renaming %s to %s.", s->service_name, t);
		free(s->service_name);
		s->service_name = t;

		publish_service(s);
		break;
	}
	case AVAHI_ENTRY_GROUP_FAILURE:
		pw_log_error("Failed to register service: %s",
				avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
		avahi_entry_group_free(g);
		s->entry_group = NULL;
		break;

	case AVAHI_ENTRY_GROUP_UNCOMMITED:
	case AVAHI_ENTRY_GROUP_REGISTERING:
		break;
	}
}

#define PA_CHANNEL_MAP_SNPRINT_MAX 336

static void publish_service(struct service *s)
{
	static const char * const subtype_text[] = {
		[SUBTYPE_HARDWARE] = "hardware",
		[SUBTYPE_VIRTUAL] = "virtual",
		[SUBTYPE_MONITOR] = "monitor"
	};

	AvahiStringList *txt = NULL;
	const char *t;
	char cm[PA_CHANNEL_MAP_SNPRINT_MAX];

	spa_assert(s);

	if (!s->userdata->client || avahi_client_get_state(s->userdata->client) != AVAHI_CLIENT_S_RUNNING)
		return;

	if (!s->entry_group) {
		if (!(s->entry_group =
		avahi_entry_group_new(s->userdata->client, service_entry_group_callback, s))) {
			pw_log_error("avahi_entry_group_new(): %s",
					avahi_strerror(avahi_client_errno(s->userdata->client)));
			goto finish;
		}
	} else
		avahi_entry_group_reset(s->entry_group);

	txt = txt_record_server_data(s->userdata->manager->info, txt);

	txt = avahi_string_list_add_pair(txt, "device", s->name);
	txt = avahi_string_list_add_printf(txt, "rate=%u", s->ss.rate);
	txt = avahi_string_list_add_printf(txt, "channels=%u", s->ss.channels);
	txt = avahi_string_list_add_pair(txt, "format", format_id2paname(s->ss.format));
	txt = avahi_string_list_add_pair(txt, "channel_map", channel_map_snprint(cm, sizeof(cm), &s->cm));
	txt = avahi_string_list_add_pair(txt, "subtype", subtype_text[s->subtype]);

	if ((t = pw_properties_get(s->props, PW_KEY_NODE_DESCRIPTION)))
		txt = avahi_string_list_add_pair(txt, "description", t);
	if ((t = pw_properties_get(s->props, PW_KEY_DEVICE_VENDOR_NAME)))
		txt = avahi_string_list_add_pair(txt, "vendor-name", t);
	if ((t = pw_properties_get(s->props, PW_KEY_DEVICE_PRODUCT_NAME)))
		txt = avahi_string_list_add_pair(txt, "product-name", t);
	if ((t = pw_properties_get(s->props, PW_KEY_DEVICE_CLASS)))
		txt = avahi_string_list_add_pair(txt, "class", t);
	if ((t = pw_properties_get(s->props, PW_KEY_DEVICE_FORM_FACTOR)))
		txt = avahi_string_list_add_pair(txt, "form-factor", t);
	if ((t = pw_properties_get(s->props, PW_KEY_DEVICE_ICON_NAME)))
		txt = avahi_string_list_add_pair(txt, "icon-name", t);

	if (avahi_entry_group_add_service_strlst(
				s->entry_group,
				AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
				0,
				s->service_name,
				s->service_type,
				NULL,
				NULL,
				s->userdata->port,
				txt) < 0) {
		pw_log_error("avahi_entry_group_add_service_strlst(): %s",
			avahi_strerror(avahi_client_errno(s->userdata->client)));
		goto finish;
	}

	if (avahi_entry_group_add_service_subtype(
				s->entry_group,
				AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
				0,
				s->service_name,
				s->service_type,
				NULL,
				s->is_sink ? (s->subtype == SUBTYPE_HARDWARE ? SERVICE_SUBTYPE_SINK_HARDWARE : SERVICE_SUBTYPE_SINK_VIRTUAL) :
				(s->subtype == SUBTYPE_HARDWARE ? SERVICE_SUBTYPE_SOURCE_HARDWARE : (s->subtype == SUBTYPE_VIRTUAL ? SERVICE_SUBTYPE_SOURCE_VIRTUAL : SERVICE_SUBTYPE_SOURCE_MONITOR))) < 0) {

		pw_log_error("avahi_entry_group_add_service_subtype(): %s",
			avahi_strerror(avahi_client_errno(s->userdata->client)));
		goto finish;
	}

	if (!s->is_sink && s->subtype != SUBTYPE_MONITOR) {
		if (avahi_entry_group_add_service_subtype(
					s->entry_group,
					AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
					0,
					s->service_name,
					SERVICE_TYPE_SOURCE,
					NULL,
					SERVICE_SUBTYPE_SOURCE_NON_MONITOR) < 0) {
			pw_log_error("avahi_entry_group_add_service_subtype(): %s",
					avahi_strerror(avahi_client_errno(s->userdata->client)));
					goto finish;
		}
	}

	if (avahi_entry_group_commit(s->entry_group) < 0) {
		pw_log_error("avahi_entry_group_commit(): %s",
				avahi_strerror(avahi_client_errno(s->userdata->client)));
		goto finish;
	}

	pw_log_info("Successfully created entry group for %s.", s->service_name);

finish:
	avahi_string_list_free(txt);
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *d)
{
	struct module_zeroconf_publish_data *data = d;

	spa_assert(c);
	spa_assert(data);

	data->client = c;

	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		break;
	case AVAHI_CLIENT_S_COLLISION:
		pw_log_error("Host name collision");
		unpublish_all_services(d, false);
		break;
	case AVAHI_CLIENT_FAILURE:
		if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
			int error;

			pw_log_debug("Avahi daemon disconnected.");

			unpublish_all_services(d, true);
			avahi_client_free(data->client);

			if (!(data->client = avahi_client_new(data->avahi_poll,
					AVAHI_CLIENT_NO_FAIL, client_callback, data, &error))) {
				pw_log_error("avahi_client_new() failed: %s",
						avahi_strerror(error));
				module_schedule_unload(data->module);
			}
		}
		break;
	default:
		break;
	}
}

static void manager_removed(void *d, struct pw_manager_object *o)
{
	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source(o))
		return;

	service_free(d, o);
}

static void manager_added(void *d, struct pw_manager_object *o)
{
	struct service *s;

	if (!pw_manager_object_is_sink(o) && !pw_manager_object_is_source(o))
		return;

	s = get_service(d, o);

	publish_service(s);
}

static const struct pw_manager_events manager_events = {
	PW_VERSION_MANAGER_EVENTS,
	.added = manager_added,
	.removed = manager_removed,
};

static int module_zeroconf_publish_load(struct client *client, struct module *module)
{
	struct module_zeroconf_publish_data *data = module->user_data;
	struct pw_loop *loop;
	int error;

	data->core = pw_context_connect(module->impl->context,
			pw_properties_copy(client->props), 0);
	if (data->core == NULL) {
		pw_log_error("Failed to connect to pipewire context");
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
		pw_log_error("avahi_client_new() failed: %s", avahi_strerror(error));
		pw_avahi_poll_free(data->avahi_poll);
		return -errno;
	}

	data->manager = pw_manager_new(data->core);
	if (client->manager == NULL) {
		pw_log_error("Failed to create pipewire manager");
		avahi_client_free(data->client);
		pw_avahi_poll_free(data->avahi_poll);
		return -errno;
	}

	pw_manager_add_listener(data->manager, &data->manager_listener,
			&manager_events, data);

	return 0;
}

static int module_zeroconf_publish_unload(struct client *client, struct module *module)
{
	struct module_zeroconf_publish_data *d = module->user_data;

	pw_manager_for_each_object(d->manager, service_free, d);

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

static const struct module_methods module_zeroconf_publish_methods = {
	VERSION_MODULE_METHODS,
	.load = module_zeroconf_publish_load,
	.unload = module_zeroconf_publish_unload,
};

static const struct spa_dict_item module_zeroconf_publish_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io" },
	{ PW_KEY_MODULE_DESCRIPTION, "mDNS/DNS-SD Service Publish" },
	{ PW_KEY_MODULE_USAGE, "port=<TCP port number>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_zeroconf_publish(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_zeroconf_publish_data *d;
	struct pw_properties *props = NULL;
	const char *port;
	int res;

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_zeroconf_publish_info));
	if (!props) {
		res = -errno;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	module = module_new(impl, &module_zeroconf_publish_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;

	if ((port = pw_properties_get(props, "port")) == NULL)
		d->port = PW_PROTOCOL_PULSE_DEFAULT_PORT;
	else
		d->port = atoi(port);


	return module;
out:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}
