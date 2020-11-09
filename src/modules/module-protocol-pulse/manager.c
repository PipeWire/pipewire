/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include "manager.h"

#include <spa/pod/iter.h>
#include <extensions/metadata.h>

#define manager_emit_sync(m) spa_hook_list_call(&m->hooks, struct pw_manager_events, sync, 0)
#define manager_emit_added(m,o) spa_hook_list_call(&m->hooks, struct pw_manager_events, added, 0, o)
#define manager_emit_updated(m,o) spa_hook_list_call(&m->hooks, struct pw_manager_events, updated, 0, o)
#define manager_emit_removed(m,o) spa_hook_list_call(&m->hooks, struct pw_manager_events, removed, 0, o)
#define manager_emit_metadata(m,s,k,t,v) spa_hook_list_call(&m->hooks, struct pw_manager_events, metadata,0,s,k,t,v)

struct object;

struct manager {
	struct pw_manager this;

	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	int sync_seq;

	struct object *metadata;

	struct spa_hook_list hooks;
};

struct object_info {
	const char *type;
	uint32_t version;
	const void *events;
	void (*init) (struct object *object);
	void (*destroy) (struct object *object);
};

struct object {
	struct pw_manager_object this;

	struct manager *manager;

	const struct object_info *info;

	struct spa_hook proxy_listener;
	struct spa_hook object_listener;

	unsigned int new:1;
};

static void core_sync(struct manager *m)
{
	m->sync_seq = pw_core_sync(m->this.core, PW_ID_CORE, m->sync_seq);
}

static struct pw_manager_param *add_param(struct spa_list *params, uint32_t id, const struct spa_pod *param)
{
	struct pw_manager_param *p;

	if (param == NULL || !spa_pod_is_object(param)) {
		errno = EINVAL;
		return NULL;
	}
	if (id == SPA_ID_INVALID)
		id = SPA_POD_OBJECT_ID(param);

	p = malloc(sizeof(*p) + SPA_POD_SIZE(param));
	if (p == NULL)
		return NULL;

	p->id = id;
	p->param = SPA_MEMBER(p, sizeof(*p), struct spa_pod);
	memcpy(p->param, param, SPA_POD_SIZE(param));
	spa_list_append(params, &p->link);

	return p;
}

static uint32_t clear_params(struct spa_list *param_list, uint32_t id)
{
	struct pw_manager_param *p, *t;
	uint32_t count = 0;

	spa_list_for_each_safe(p, t, param_list, link) {
		if (id == SPA_ID_INVALID || p->id == id) {
			spa_list_remove(&p->link);
			free(p);
			count++;
		}
	}
	return count;
}


static struct object *find_object(struct manager *m, uint32_t id)
{
	struct object *o;
	spa_list_for_each(o, &m->this.object_list, this.link) {
		if (o->this.id == id)
			return o;
	}
	return NULL;
}

static void object_destroy(struct object *o)
{
	struct manager *m = o->manager;
	spa_list_remove(&o->this.link);
	m->this.n_objects--;
	if (o->this.proxy)
		pw_proxy_destroy(o->this.proxy);
	if (o->this.props)
		pw_properties_free(o->this.props);
	clear_params(&o->this.param_list, SPA_ID_INVALID);
	free(o);
}

/* client */
static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct object *o = object;
	int changed = 0;

        pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->this.id, info->change_mask);

        info = o->this.info = pw_client_info_update(o->this.info, info);

	if (info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS)
		changed++;

	if (changed) {
		o->this.changed += changed;
		core_sync(o->manager);
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
};

static void client_destroy(struct object *o)
{
	if (o->this.info)
		pw_client_info_free(o->this.info);
}

static const struct object_info client_info = {
	.type = PW_TYPE_INTERFACE_Client,
	.version = PW_VERSION_CLIENT,
	.events = &client_events,
	.destroy = client_destroy,
};

/* module */
static void module_event_info(void *object, const struct pw_module_info *info)
{
        struct object *o = object;
	int changed = 0;

        pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->this.id, info->change_mask);

        info = o->this.info = pw_module_info_update(o->this.info, info);

	if (info->change_mask & PW_MODULE_CHANGE_MASK_PROPS)
		changed++;

	if (changed) {
		o->this.changed += changed;
		core_sync(o->manager);
	}
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.info = module_event_info,
};

static void module_destroy(struct object *o)
{
	if (o->this.info)
		pw_module_info_free(o->this.info);
}

static const struct object_info module_info = {
	.type = PW_TYPE_INTERFACE_Module,
	.version = PW_VERSION_MODULE,
	.events = &module_events,
	.destroy = module_destroy,
};

/* device */
static void device_event_info(void *object, const struct pw_device_info *info)
{
	struct object *o = object;
	uint32_t i, changed = 0;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->this.id, info->change_mask);

	info = o->this.info = pw_device_info_update(o->this.info, info);

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS)
		changed++;

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t id = info->params[i].id;

			if (info->params[i].user == 0)
				continue;
			info->params[i].user = 0;

			changed++;
			clear_params(&o->this.param_list, id);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;

			pw_device_enum_params((struct pw_device*)o->this.proxy,
					0, id, 0, -1, NULL);
		}
	}
	if (changed) {
		o->this.changed += changed;
		core_sync(o->manager);
	}
}

static void device_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct object *o = object;
	add_param(&o->this.param_list, id, param);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static void device_destroy(struct object *o)
{
	if (o->this.info)
		pw_device_info_free(o->this.info);
}

static const struct object_info device_info = {
	.type = PW_TYPE_INTERFACE_Device,
	.version = PW_VERSION_DEVICE,
	.events = &device_events,
	.destroy = device_destroy,
};

/* node */
static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct object *o = object;
	uint32_t i, changed = 0;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->this.id, info->change_mask);

	info = o->this.info = pw_node_info_update(o->this.info, info);

	if (info->change_mask & PW_NODE_CHANGE_MASK_STATE)
		changed++;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
		changed++;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t id = info->params[i].id;

			if (info->params[i].user == 0)
				continue;
			info->params[i].user = 0;

			changed++;
			clear_params(&o->this.param_list, id);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;

			pw_node_enum_params((struct pw_node*)o->this.proxy,
					0, id, 0, -1, NULL);
		}
	}
	if (changed) {
		o->this.changed += changed;
		core_sync(o->manager);
	}
}

static void node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct object *o = object;
	add_param(&o->this.param_list, id, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void node_destroy(struct object *o)
{
	if (o->this.info)
		pw_node_info_free(o->this.info);
}

static const struct object_info node_info = {
	.type = PW_TYPE_INTERFACE_Node,
	.version = PW_VERSION_NODE,
	.events = &node_events,
	.destroy = node_destroy,
};

/* link */
static const struct object_info link_info = {
	.type = PW_TYPE_INTERFACE_Link,
	.version = PW_VERSION_LINK,
};

/* metadata */
static int metadata_property(void *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
	struct object *o = object;
	struct manager *m = o->manager;
	manager_emit_metadata(m, subject, key, type, value);
        return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void metadata_init(struct object *o)
{
	struct manager *m = o->manager;
	if (m->metadata == NULL)
		m->metadata = o;
}

static void metadata_destroy(struct object *o)
{
	struct manager *m = o->manager;
	if (m->metadata == o)
		m->metadata = NULL;
}

static const struct object_info metadata_info = {
	.type = PW_TYPE_INTERFACE_Metadata,
	.version = PW_VERSION_METADATA,
	.events = &metadata_events,
	.init = metadata_init,
	.destroy = metadata_destroy,
};

static const struct object_info *objects[] =
{
	&module_info,
	&client_info,
	&device_info,
	&node_info,
	&link_info,
	&metadata_info,
};

static const struct object_info *find_info(const char *type, uint32_t version)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(objects); i++) {
		if (strcmp(objects[i]->type, type) == 0 &&
		    objects[i]->version <= version)
			return objects[i];
	}
	return NULL;
}

static void
destroy_removed(void *data)
{
	struct object *o = data;
	pw_proxy_destroy(o->this.proxy);
}

static void
destroy_proxy(void *data)
{
	struct object *o = data;

	if (o->info->events)
		spa_hook_remove(&o->object_listener);
	spa_hook_remove(&o->proxy_listener);

	if (o->info && o->info->destroy)
                o->info->destroy(o);

        o->this.proxy = NULL;
}

static const struct pw_proxy_events proxy_events = {
        PW_VERSION_PROXY_EVENTS,
        .removed = destroy_removed,
        .destroy = destroy_proxy,
};

static void registry_event_global(void *data, uint32_t id,
			uint32_t permissions, const char *type, uint32_t version,
			const struct spa_dict *props)
{
	struct manager *m = data;
	struct object *o;
	const struct object_info *info;
	struct pw_proxy *proxy;

	info = find_info(type, version);
	if (info == NULL)
		return;

	proxy = pw_registry_bind(m->this.registry,
			id, type, info->version, 0);
        if (proxy == NULL)
		return;

	o = calloc(1, sizeof(*o));
	if (o == NULL) {
		pw_log_error("can't alloc object for %u %s/%d: %m", id, type, version);
		pw_proxy_destroy(proxy);
		return;
	}
	o->this.id = id;
	o->this.permissions = permissions;
	o->this.type = info->type;
	o->this.version = version;
	o->this.props = props ? pw_properties_new_dict(props) : NULL;
	o->this.proxy = proxy;
	o->new = true;
	spa_list_init(&o->this.param_list);

	o->manager = m;
	o->info = info;
	spa_list_append(&m->this.object_list, &o->this.link);
	m->this.n_objects++;

	if (info->events)
		pw_proxy_add_object_listener(proxy,
				&o->object_listener,
				o->info->events, o);
	pw_proxy_add_listener(proxy,
			&o->proxy_listener,
			&proxy_events, o);

	if (info->init)
		info->init(o);

	core_sync(m);
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	struct manager *m = object;
	struct object *o;

	if ((o = find_object(m, id)) == NULL)
		return;

	manager_emit_removed(m, &o->this);

	object_destroy(o);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_info(void *data, const struct pw_core_info *info)
{
	struct manager *m = data;
	m->this.info = pw_core_info_update(m->this.info, info);
}

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct manager *m = data;
	struct object *o;

	if (id == PW_ID_CORE) {
		if (m->sync_seq == seq)
			manager_emit_sync(m);

		spa_list_for_each(o, &m->this.object_list, this.link) {
			if (o->new) {
				o->new = false;
				manager_emit_added(m, &o->this);
			} else if (o->this.changed > 0) {
				manager_emit_updated(m, &o->this);
				o->this.changed = 0;
			}
		}
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.info = on_core_info,
};

struct pw_manager *pw_manager_new(struct pw_core *core)
{
	struct manager *m;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return NULL;

	m->this.core = core;
	m->this.registry = pw_core_get_registry(m->this.core,
			PW_VERSION_REGISTRY, 0);
	if (m->this.registry == NULL) {
		free(m);
		return NULL;
	}

	spa_hook_list_init(&m->hooks);

	spa_list_init(&m->this.object_list);

	pw_core_add_listener(m->this.core,
			&m->core_listener,
			&core_events, m);
	pw_registry_add_listener(m->this.registry,
			&m->registry_listener,
			&registry_events, m);

	return &m->this;
}

void pw_manager_add_listener(struct pw_manager *manager,
		struct spa_hook *listener,
		const struct pw_manager_events *events, void *data)
{
	struct manager *m = SPA_CONTAINER_OF(manager, struct manager, this);
	spa_hook_list_append(&m->hooks, listener, events, data);
	core_sync(m);
}

int pw_manager_set_metadata(struct pw_manager *manager,
		uint32_t subject, const char *key, const char *type,
		const char *format, ...)
{
	struct manager *m = SPA_CONTAINER_OF(manager, struct manager, this);
	struct object *s;
	va_list args;
	char buf[1024];

	if ((s = find_object(m, subject)) == NULL)
		return -ENOENT;
	if (!SPA_FLAG_IS_SET(s->this.permissions, PW_PERM_M))
		return -EACCES;

	if (m->metadata == NULL)
		return -ENOTSUP;
	if (!SPA_FLAG_IS_SET(m->metadata->this.permissions, PW_PERM_W|PW_PERM_X))
		return -EACCES;

        va_start(args, format);
	vsnprintf(buf, sizeof(buf)-1, format, args);
        va_end(args);

	pw_metadata_set_property(m->metadata->this.proxy,
			subject, key, type, buf);
	return 0;
}

int pw_manager_for_each_object(struct pw_manager *manager,
		int (*callback) (void *data, struct pw_manager_object *object),
		void *data)
{
	struct manager *m = SPA_CONTAINER_OF(manager, struct manager, this);
	struct object *o;
	int res;

	spa_list_for_each(o, &m->this.object_list, this.link) {
		if ((res = callback(data, &o->this)) != 0)
			return res;
	}
	return 0;
}

void pw_manager_destroy(struct pw_manager *manager)
{
	struct manager *m = SPA_CONTAINER_OF(manager, struct manager, this);
	struct object *o;

	spa_hook_remove(&m->core_listener);

	spa_list_consume(o, &m->this.object_list, this.link)
		object_destroy(o);

	spa_hook_remove(&m->registry_listener);
	pw_proxy_destroy((struct pw_proxy*)m->this.registry);

	if (m->this.info)
		pw_core_info_free(m->this.info);

	free(m);
}
