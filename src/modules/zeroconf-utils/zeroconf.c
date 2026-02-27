/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/json.h>

#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include "avahi-poll.h"
#include "zeroconf.h"

#define pw_zeroconf_emit(o,m,v,...) 	spa_hook_list_call(&o->listener_list, struct pw_zeroconf_events, m, v, ##__VA_ARGS__)
#define pw_zeroconf_emit_destroy(c)		pw_zeroconf_emit(c, destroy, 0)
#define pw_zeroconf_emit_error(c,e,m)		pw_zeroconf_emit(c, error, 0, e, m)
#define pw_zeroconf_emit_added(c,id,i)		pw_zeroconf_emit(c, added, 0, id, i)
#define pw_zeroconf_emit_removed(c,id,i)	pw_zeroconf_emit(c, removed, 0, id, i)

struct service_info {
	AvahiIfIndex interface;
	AvahiProtocol protocol;
	const char *name;
	const char *type;
	const char *domain;
	const char *host_name;
	AvahiAddress address;
	uint16_t port;
};

#define SERVICE_INFO(...) ((struct service_info){ __VA_ARGS__ })

#define STR_TO_PROTO(s)  (atoi(s) == 6 ? AVAHI_PROTO_INET6 : AVAHI_PROTO_INET)

struct entry {
	struct pw_zeroconf *zc;
	struct spa_list link;

#define TYPE_ANNOUNCE	0
#define TYPE_BROWSE	1
	uint32_t type;
	const void *user;

	struct pw_properties *props;

	AvahiEntryGroup *group;
	AvahiServiceBrowser *browser;

	struct spa_list services;
};

struct service {
	struct entry *e;
	struct spa_list link;

	struct service_info info;

	struct pw_properties *props;
};

struct pw_zeroconf {
	int refcount;
	struct pw_context *context;

	struct pw_properties *props;

	struct spa_hook_list listener_list;

	AvahiPoll *poll;
	AvahiClient *client;
	AvahiClientState state;

	struct spa_list entries;

	bool discover_local;
};

static struct service *service_find(struct entry *e, const struct service_info *info)
{
	struct service *s;
	spa_list_for_each(s, &e->services, link) {
		if (s->info.interface == info->interface &&
		    s->info.protocol == info->protocol &&
		    spa_streq(s->info.name, info->name) &&
		    spa_streq(s->info.type, info->type) &&
		    spa_streq(s->info.domain, info->domain))
			return s;
	}
	return NULL;
}

static void service_free(struct service *s)
{
	spa_list_remove(&s->link);
	free((void*)s->info.name);
	free((void*)s->info.type);
	free((void*)s->info.domain);
	free((void*)s->info.host_name);
	pw_properties_free(s->props);
	free(s);
}

struct entry *entry_find(struct pw_zeroconf *zc, uint32_t type, const void *user)
{
	struct entry *e;
	spa_list_for_each(e, &zc->entries, link)
		if (e->type == type && e->user == user)
			return e;
	return NULL;
}

static void entry_free(struct entry *e)
{
	struct service *s;

	spa_list_remove(&e->link);
	if (e->group)
		avahi_entry_group_free(e->group);
	spa_list_consume(s, &e->services, link)
		service_free(s);
	pw_properties_free(e->props);
	free(e);
}

static void zeroconf_free(struct pw_zeroconf *zc)
{
	struct entry *a;

	spa_list_consume(a, &zc->entries, link)
		entry_free(a);

	if (zc->client)
                avahi_client_free(zc->client);
	if (zc->poll)
		pw_avahi_poll_free(zc->poll);
	pw_properties_free(zc->props);
	free(zc);
}

static void zeroconf_unref(struct pw_zeroconf *zc)
{
	if (--zc->refcount == 0)
		zeroconf_free(zc);
}

void pw_zeroconf_destroy(struct pw_zeroconf *zc)
{
	pw_zeroconf_emit_destroy(zc);

	zeroconf_unref(zc);
}

static struct service *service_new(struct entry *e,
		const struct service_info *info, AvahiStringList *txt)
{
	struct service *s;
	struct pw_zeroconf *zc = e->zc;
	const AvahiAddress *a = &info->address;
	static const char *link_local_range = "169.254.";
	AvahiStringList *l;
	char at[AVAHI_ADDRESS_STR_MAX], if_suffix[16] = "";

	if ((s = calloc(1, sizeof(*s))) == NULL)
		goto error;

	s->e = e;
	spa_list_append(&e->services, &s->link);

	s->info.interface = info->interface;
	s->info.protocol = info->protocol;
	s->info.name = strdup(info->name);
	s->info.type = strdup(info->type);
	s->info.domain = strdup(info->domain);
	s->info.host_name = strdup(info->host_name);
	s->info.address = info->address;
	s->info.port = info->port;

	if ((s->props = pw_properties_new(NULL, NULL)) == NULL)
		goto error;

	if (a->proto == AVAHI_PROTO_INET6 && info->interface != AVAHI_IF_UNSPEC &&
	    a->data.ipv6.address[0] == 0xfe &&
	    (a->data.ipv6.address[1] & 0xc0) == 0x80)
		snprintf(if_suffix, sizeof(if_suffix), "%%%d", info->interface);

	avahi_address_snprint(at, sizeof(at), a);
	if (a->proto == AVAHI_PROTO_INET && info->interface != AVAHI_IF_UNSPEC &&
	    spa_strstartswith(at, link_local_range))
		snprintf(if_suffix, sizeof(if_suffix), "%%%d", info->interface);

	if (info->interface != AVAHI_IF_UNSPEC)
		pw_properties_setf(s->props, PW_KEY_ZEROCONF_IFINDEX, "%d", info->interface);
	if (a->proto != AVAHI_PROTO_UNSPEC)
		pw_properties_set(s->props, PW_KEY_ZEROCONF_PROTO,
				a->proto == AVAHI_PROTO_INET ? "4" : "6");

	pw_properties_set(s->props, PW_KEY_ZEROCONF_NAME, info->name);
	pw_properties_set(s->props, PW_KEY_ZEROCONF_TYPE, info->type);
	pw_properties_set(s->props, PW_KEY_ZEROCONF_DOMAIN, info->domain);
	pw_properties_set(s->props, PW_KEY_ZEROCONF_HOSTNAME, info->host_name);
	pw_properties_setf(s->props, PW_KEY_ZEROCONF_ADDRESS, "%s%s", at, if_suffix);
	pw_properties_setf(s->props, PW_KEY_ZEROCONF_PORT, "%u", info->port);

	for (l = txt; l; l = l->next) {
		char *key, *value;

		if (avahi_string_list_get_pair(l, &key, &value, NULL) != 0)
			break;

		pw_properties_set(s->props, key, value);
		avahi_free(key);
		avahi_free(value);
	}

	pw_log_info("new %s %s %s %s", info->name, info->type, info->domain, info->host_name);
	pw_zeroconf_emit_added(zc, e->user, &s->props->dict);

	return s;

error:
	if (s)
		service_free(s);
	return NULL;
}

static void resolver_cb(AvahiServiceResolver *r, AvahiIfIndex interface,
		AvahiProtocol protocol, AvahiResolverEvent event,
		const char *name, const char *type, const char *domain,
		const char *host_name, const AvahiAddress *a, uint16_t port,
		AvahiStringList *txt, AvahiLookupResultFlags flags,
		void *userdata)
{
	struct entry *e = userdata;
	struct pw_zeroconf *zc = e->zc;
	struct service_info info;

	if (event != AVAHI_RESOLVER_FOUND) {
		pw_log_error("Resolving of '%s' failed: %s", name,
				avahi_strerror(avahi_client_errno(zc->client)));
		goto done;
	}

	info = SERVICE_INFO(.interface = interface,
			.protocol = protocol,
			.name = name,
			.type = type,
			.domain = domain,
			.host_name = host_name,
			.address = *a,
			.port = port);

	service_new(e, &info, txt);
done:
	avahi_service_resolver_free(r);
}

static void browser_cb(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
	AvahiLookupResultFlags flags, void *userdata)
{
        struct entry *e = userdata;
	struct pw_zeroconf *zc = e->zc;
	struct service_info info;
	struct service *s;
	int aproto = AVAHI_PROTO_UNSPEC;
	const char *str;

	if ((flags & AVAHI_LOOKUP_RESULT_LOCAL) && !zc->discover_local)
		return;

	info = SERVICE_INFO(.interface = interface,
			.protocol = protocol,
			.name = name,
			.type = type,
			.domain = domain);

	s = service_find(e, &info);

	switch (event) {
	case AVAHI_BROWSER_NEW:
		if (s != NULL)
			return;

		if ((str = pw_properties_get(e->props, PW_KEY_ZEROCONF_RESOLVE_PROTO)))
			aproto = STR_TO_PROTO(str);

		if (!(avahi_service_resolver_new(zc->client,
						interface, protocol,
						name, type, domain,
						aproto, 0,
						resolver_cb, e))) {
			int res = avahi_client_errno(zc->client);
			pw_log_error("can't make service resolver: %s", avahi_strerror(res));
			pw_zeroconf_emit_error(zc, res, avahi_strerror(res));
		}
		break;
	case AVAHI_BROWSER_REMOVE:
		if (s == NULL)
			return;
		pw_log_info("removed %s %s %s", name, type, domain);
		pw_zeroconf_emit_removed(zc, e->user, &s->props->dict);
		service_free(s);
		break;
	default:
		break;
	}
}

static int do_browse(struct pw_zeroconf *zc, struct entry *e)
{
	const struct spa_dict_item *it;
	const char *type = NULL, *domain = NULL;
	int res, ifindex = AVAHI_IF_UNSPEC, proto = AVAHI_PROTO_UNSPEC;

	if (e->browser == NULL) {
		spa_dict_for_each(it, &e->props->dict) {
			if (spa_streq(it->key, PW_KEY_ZEROCONF_IFINDEX))
				ifindex = atoi(it->value);
			else if (spa_streq(it->key, PW_KEY_ZEROCONF_PROTO))
				proto = STR_TO_PROTO(it->value);
			else if (spa_streq(it->key, PW_KEY_ZEROCONF_TYPE))
				type = it->value;
			else if (spa_streq(it->key, PW_KEY_ZEROCONF_DOMAIN))
				domain = it->value;
		}
		if (type == NULL) {
			res = -EINVAL;
			pw_log_error("can't make browser: no "PW_KEY_ZEROCONF_TYPE" provided");
			pw_zeroconf_emit_error(zc, res, spa_strerror(res));
			return res;
		}
		e->browser = avahi_service_browser_new(zc->client,
				ifindex, proto, type, domain, 0,
				browser_cb, e);
	        if (e->browser == NULL) {
			res = avahi_client_errno(zc->client);
			pw_log_error("can't make browser: %s", avahi_strerror(res));
			pw_zeroconf_emit_error(zc, res, avahi_strerror(res));
			return -EIO;
		}
	}
	return 0;
}

static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata)
{
	struct entry *e = userdata;
	struct pw_zeroconf *zc = e->zc;
	const char *name;
	int res;

	zc->refcount++;

	name = pw_properties_get(e->props, PW_KEY_ZEROCONF_NAME);

	switch (state) {
	case AVAHI_ENTRY_GROUP_ESTABLISHED:
		pw_log_debug("Entry \"%s\" added", name);
		break;
	case AVAHI_ENTRY_GROUP_COLLISION:
		pw_log_error("Entry \"%s\" name collision", name);
		break;
	case AVAHI_ENTRY_GROUP_FAILURE:
		res = avahi_client_errno(zc->client);
		pw_log_error("Entry \"%s\" failure: %s", name, avahi_strerror(res));
		pw_zeroconf_emit_error(zc, res, avahi_strerror(res));
		break;
	case AVAHI_ENTRY_GROUP_UNCOMMITED:
	case AVAHI_ENTRY_GROUP_REGISTERING:
		break;
	}
	zeroconf_unref(zc);
}

static int do_announce(struct pw_zeroconf *zc, struct entry *e)
{
	AvahiStringList *txt = NULL;
	int res, ifindex = AVAHI_IF_UNSPEC, proto = AVAHI_PROTO_UNSPEC;
	const struct spa_dict_item *it;
	const char *name = "unnamed", *type = NULL, *subtypes = NULL;
	const char *domain = NULL, *host = NULL;
	uint16_t port = 0;

	if (e->group == NULL) {
		e->group = avahi_entry_group_new(zc->client,
					entry_group_callback, e);
	        if (e->group == NULL) {
			res = avahi_client_errno(zc->client);
			pw_log_error("can't make group: %s", avahi_strerror(res));
			pw_zeroconf_emit_error(zc, res, avahi_strerror(res));
			return -EIO;
		}
	}
	avahi_entry_group_reset(e->group);

	spa_dict_for_each(it, &e->props->dict) {
		if (spa_streq(it->key, PW_KEY_ZEROCONF_IFINDEX))
			ifindex = atoi(it->value);
		else if (spa_streq(it->key, PW_KEY_ZEROCONF_PROTO))
			proto = STR_TO_PROTO(it->value);
		else if (spa_streq(it->key, PW_KEY_ZEROCONF_NAME))
			name = it->value;
		else if (spa_streq(it->key, PW_KEY_ZEROCONF_TYPE))
			type = it->value;
		else if (spa_streq(it->key, PW_KEY_ZEROCONF_DOMAIN))
			domain = it->value;
		else if (spa_streq(it->key, PW_KEY_ZEROCONF_HOST))
			host = it->value;
		else if (spa_streq(it->key, PW_KEY_ZEROCONF_PORT))
			port = atoi(it->value);
		else if (spa_streq(it->key, PW_KEY_ZEROCONF_SUBTYPES))
			subtypes = it->value;
		else if (!spa_strstartswith(it->key, "zeroconf."))
			txt = avahi_string_list_add_pair(txt, it->key, it->value);
	}
	if (type == NULL) {
		res = -EINVAL;
		pw_log_error("can't announce: no "PW_KEY_ZEROCONF_TYPE" provided");
		pw_zeroconf_emit_error(zc, res, spa_strerror(res));
		avahi_string_list_free(txt);
		return res;
	}
	res = avahi_entry_group_add_service_strlst(e->group,
			ifindex, proto,
			(AvahiPublishFlags)0, name,
			type, domain, host, port, txt);
	avahi_string_list_free(txt);

	if (res < 0) {
		res = avahi_client_errno(zc->client);
		pw_log_error("can't add service: %s", avahi_strerror(res));
		pw_zeroconf_emit_error(zc, res, avahi_strerror(res));
		return -EIO;
	}

	if (subtypes) {
		struct spa_json iter;
		char v[512];

		if (spa_json_begin_array_relax(&iter, subtypes, strlen(subtypes)) <= 0) {
			res = -EINVAL;
			pw_log_error("invalid subtypes: %s", subtypes);
			pw_zeroconf_emit_error(zc, res, spa_strerror(res));
			return res;
		}
		while (spa_json_get_string(&iter, v, sizeof(v)) > 0) {
			res = avahi_entry_group_add_service_subtype(e->group,
					ifindex, proto,
					(AvahiPublishFlags)0, name,
					type, domain, v);
			if (res < 0) {
				res = avahi_client_errno(zc->client);
				pw_log_error("can't add subtype %s: %s", v, avahi_strerror(res));
				pw_zeroconf_emit_error(zc, res, avahi_strerror(res));
				return -EIO;
			}
		}
	}
	if ((res = avahi_entry_group_commit(e->group)) < 0) {
		res = avahi_client_errno(zc->client);
		pw_log_error("can't commit group: %s", avahi_strerror(res));
		pw_zeroconf_emit_error(zc, res, avahi_strerror(res));
		return -EIO;
	}
	return 0;
}

static int entry_start(struct pw_zeroconf *zc, struct entry *e)
{
	if (zc->state != AVAHI_CLIENT_S_REGISTERING &&
	    zc->state != AVAHI_CLIENT_S_RUNNING &&
	    zc->state != AVAHI_CLIENT_S_COLLISION)
		return 0;

	if (e->type == TYPE_ANNOUNCE)
		return do_announce(zc, e);
	else
		return do_browse(zc, e);
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *d)
{
	struct pw_zeroconf *zc = d;
	struct entry *e;

	zc->client = c;
	zc->refcount++;
	zc->state = state;

	switch (state) {
	case AVAHI_CLIENT_S_REGISTERING:
	case AVAHI_CLIENT_S_RUNNING:
	case AVAHI_CLIENT_S_COLLISION:
		spa_list_for_each(e, &zc->entries, link)
			entry_start(zc, e);
		break;
	case AVAHI_CLIENT_FAILURE:
	{
                int err = avahi_client_errno(c);
		pw_zeroconf_emit_error(zc, err, avahi_strerror(err));
		break;
	}
	case AVAHI_CLIENT_CONNECTING:
	default:
		break;
	}
	zeroconf_unref(zc);
}

static struct entry *entry_new(struct pw_zeroconf *zc, uint32_t type, const void *user, const struct spa_dict *info)
{
	struct entry *e;

	if ((e = calloc(1, sizeof(*e))) == NULL)
		return NULL;

	e->zc = zc;
	e->type = type;
	e->user = user;
	e->props = pw_properties_new_dict(info);
	spa_list_append(&zc->entries, &e->link);
	spa_list_init(&e->services);

	if (type == TYPE_ANNOUNCE)
		pw_log_debug("created announce for \"%s\"",
				pw_properties_get(e->props, PW_KEY_ZEROCONF_NAME));
	else
		pw_log_debug("created browse for \"%s\"",
				pw_properties_get(e->props, PW_KEY_ZEROCONF_TYPE));
	return e;
}

static int set_entry(struct pw_zeroconf *zc, uint32_t type, const void *user, const struct spa_dict *info)
{
	struct entry *e;
	int res = 0;

	e = entry_find(zc, type, user);
	if (e == NULL) {
		if (info == NULL)
			return 0;
		if ((e = entry_new(zc, type, user, info)) == NULL)
			return -errno;
		res = entry_start(zc, e);
	} else {
		if (info == NULL)
			entry_free(e);
		else {
			pw_properties_update(e->props, info);
			res = entry_start(zc, e);
		}
	}
	return res;
}
int pw_zeroconf_set_announce(struct pw_zeroconf *zc, const void *user, const struct spa_dict *info)
{
	return set_entry(zc, TYPE_ANNOUNCE, user, info);
}
int pw_zeroconf_set_browse(struct pw_zeroconf *zc, const void *user, const struct spa_dict *info)
{
	return set_entry(zc, TYPE_BROWSE, user, info);
}

struct pw_zeroconf * pw_zeroconf_new(struct pw_context *context,
				struct spa_dict *props)
{
	struct pw_zeroconf *zc;
	uint32_t i;
	int res;

	if ((zc = calloc(1, sizeof(*zc))) == NULL)
		return NULL;

	zc->refcount = 1;
	zc->context = context;
	spa_hook_list_init(&zc->listener_list);
	spa_list_init(&zc->entries);
	zc->props = props ? pw_properties_new_dict(props) : pw_properties_new(NULL, NULL);
	zc->discover_local = true;

	for (i = 0; props && i < props->n_items; i++) {
		const char *k = props->items[i].key;
		const char *v = props->items[i].value;

		if (spa_streq(k, PW_KEY_ZEROCONF_DISCOVER_LOCAL) && v)
			zc->discover_local = spa_atob(v);
	}

	zc->poll = pw_avahi_poll_new(context);
	if (zc->poll == NULL)
		goto error;

	zc->client = avahi_client_new(zc->poll, AVAHI_CLIENT_NO_FAIL,
			client_callback, zc, &res);
	if (!zc->client) {
		pw_log_error("failed to create avahi client: %s", avahi_strerror(res));
		goto error;
	}
	return zc;
error:
	zeroconf_free(zc);
	return NULL;
}

void pw_zeroconf_add_listener(struct pw_zeroconf *zc,
		struct spa_hook *listener,
		const struct pw_zeroconf_events *events, void *data)
{
	spa_hook_list_append(&zc->listener_list, listener, events, data);
}
