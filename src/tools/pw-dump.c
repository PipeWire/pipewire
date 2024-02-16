/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <fnmatch.h>
#include <locale.h>

#if !defined(FNM_EXTMATCH)
#define FNM_EXTMATCH 0
#endif

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/pod/iter.h>
#include <spa/debug/types.h>
#include <spa/utils/json.h>
#include <spa/utils/ansi.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#define INDENT 2

static bool colors = false;

#define NORMAL	(colors ? SPA_ANSI_RESET : "")
#define LITERAL	(colors ? SPA_ANSI_BRIGHT_MAGENTA : "")
#define NUMBER	(colors ? SPA_ANSI_BRIGHT_CYAN : "")
#define STRING	(colors ? SPA_ANSI_BRIGHT_GREEN : "")
#define KEY	(colors ? SPA_ANSI_BRIGHT_BLUE : "")

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core_info *info;
	struct pw_core *core;
	struct spa_hook core_listener;
	int sync_seq;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list object_list;

	const char *pattern;

	FILE *out;
	int level;
#define STATE_KEY	(1<<0)
#define STATE_COMMA	(1<<1)
#define STATE_FIRST	(1<<2)
#define STATE_MASK	0xffff0000
#define STATE_SIMPLE	(1<<16)
	uint32_t state;

	unsigned int monitor:1;
};

struct param {
	uint32_t id;
	int32_t seq;
	struct spa_list link;
	struct spa_pod *param;
};

struct object;

struct class {
	const char *type;
	uint32_t version;
	const void *events;
	void (*destroy) (struct object *object);
	void (*dump) (struct object *object);
	const char *name_key;
};

struct object {
	struct spa_list link;

	struct data *data;

	uint32_t id;
	uint32_t permissions;
	char *type;
	uint32_t version;
	struct pw_properties *props;

	const struct class *class;
	void *info;
	struct spa_param_info *params;
	uint32_t n_params;

	int changed;
	struct spa_list param_list;
	struct spa_list pending_list;
	struct spa_list data_list;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;
};

static void core_sync(struct data *d)
{
	d->sync_seq = pw_core_sync(d->core, PW_ID_CORE, d->sync_seq);
	pw_log_debug("sync start %u", d->sync_seq);
}

static uint32_t clear_params(struct spa_list *param_list, uint32_t id)
{
	struct param *p, *t;
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

static struct param *add_param(struct spa_list *params, int seq,
		uint32_t id, const struct spa_pod *param)
{
	struct param *p;

	if (id == SPA_ID_INVALID) {
		if (param == NULL || !spa_pod_is_object(param)) {
			errno = EINVAL;
			return NULL;
		}
		id = SPA_POD_OBJECT_ID(param);
	}

	p = malloc(sizeof(*p) + (param != NULL ? SPA_POD_SIZE(param) : 0));
	if (p == NULL)
		return NULL;

	p->id = id;
	p->seq = seq;
	if (param != NULL) {
		p->param = SPA_PTROFF(p, sizeof(*p), struct spa_pod);
		memcpy(p->param, param, SPA_POD_SIZE(param));
	} else {
		clear_params(params, id);
		p->param = NULL;
	}
	spa_list_append(params, &p->link);

	return p;
}

static struct object *find_object(struct data *d, uint32_t id)
{
	struct object *o;
	spa_list_for_each(o, &d->object_list, link) {
		if (o->id == id)
			return o;
	}
	return NULL;
}

static void object_update_params(struct spa_list *param_list, struct spa_list *pending_list,
		uint32_t n_params, struct spa_param_info *params)
{
	struct param *p, *t;
	uint32_t i;

	for (i = 0; i < n_params; i++) {
		spa_list_for_each_safe(p, t, pending_list, link) {
			if (p->id == params[i].id &&
			    p->seq != params[i].seq &&
			    p->param != NULL) {
				spa_list_remove(&p->link);
				free(p);
			}
		}
	}

	spa_list_consume(p, pending_list, link) {
		spa_list_remove(&p->link);
		if (p->param == NULL) {
			clear_params(param_list, p->id);
			free(p);
		} else {
			spa_list_append(param_list, &p->link);
		}
	}
}

static void object_destroy(struct object *o)
{
	spa_list_remove(&o->link);
	if (o->proxy)
		pw_proxy_destroy(o->proxy);
	pw_properties_free(o->props);
	clear_params(&o->param_list, SPA_ID_INVALID);
	clear_params(&o->pending_list, SPA_ID_INVALID);
	free(o->type);
	free(o);
}

static void put_key(struct data *d, const char *key);

static SPA_PRINTF_FUNC(3,4) void put_fmt(struct data *d, const char *key, const char *fmt, ...)
{
	va_list va;
	if (key)
		put_key(d, key);
	fprintf(d->out, "%s%s%*s",
			d->state & STATE_COMMA ? "," : "",
			d->state & (STATE_MASK | STATE_KEY) ? " " : d->state & STATE_FIRST ? "" : "\n",
			d->state & (STATE_MASK | STATE_KEY) ? 0 : d->level, "");
	va_start(va, fmt);
	vfprintf(d->out, fmt, va);
	va_end(va);
	d->state = (d->state & STATE_MASK) + STATE_COMMA;
}

static void put_key(struct data *d, const char *key)
{
	int size = (strlen(key) + 1) * 4;
	char *str = alloca(size);
	spa_json_encode_string(str, size, key);
	put_fmt(d, NULL, "%s%s%s:", KEY, str, NORMAL);
	d->state = (d->state & STATE_MASK) + STATE_KEY;
}

static void put_begin(struct data *d, const char *key, const char *type, uint32_t flags)
{
	put_fmt(d, key, "%s", type);
	d->level += INDENT;
	d->state = (d->state & STATE_MASK) + (flags & STATE_SIMPLE);
}

static void put_end(struct data *d, const char *type, uint32_t flags)
{
	d->level -= INDENT;
	d->state = d->state & STATE_MASK;
	put_fmt(d, NULL, "%s", type);
	d->state = (d->state & STATE_MASK) + STATE_COMMA - (flags & STATE_SIMPLE);
}

static void put_encoded_string(struct data *d, const char *key, const char *val)
{
	put_fmt(d, key, "%s%s%s", STRING, val, NORMAL);
}
static void put_string(struct data *d, const char *key, const char *val)
{
	int size = (strlen(val) + 1) * 4;
	char *str = alloca(size);
	spa_json_encode_string(str, size, val);
	put_encoded_string(d, key, str);
}

static void put_literal(struct data *d, const char *key, const char *val)
{
	put_fmt(d, key, "%s%s%s", LITERAL, val, NORMAL);
}

static void put_int(struct data *d, const char *key, int64_t val)
{
	put_fmt(d, key, "%s%"PRIi64"%s", NUMBER, val, NORMAL);
}

static void put_double(struct data *d, const char *key, double val)
{
	char buf[128];
	put_fmt(d, key, "%s%s%s", NUMBER,
			spa_json_format_float(buf, sizeof(buf), val), NORMAL);
}

static void put_value(struct data *d, const char *key, const char *val)
{
	int64_t li;
	float fv;

	if (val == NULL)
		put_literal(d, key, "null");
	else if (spa_streq(val, "true") || spa_streq(val, "false"))
		put_literal(d, key, val);
	else if (spa_atoi64(val, &li, 10))
		put_int(d, key, li);
	else if (spa_json_parse_float(val, strlen(val), &fv))
		put_double(d, key, fv);
	else
		put_string(d, key, val);
}

static void put_dict(struct data *d, const char *key, struct spa_dict *dict)
{
	const struct spa_dict_item *it;
	spa_dict_qsort(dict);
	put_begin(d, key, "{", 0);
	spa_dict_for_each(it, dict)
		put_value(d, it->key, it->value);
	put_end(d, "}", 0);
}

static void put_pod_value(struct data *d, const char *key, const struct spa_type_info *info,
		uint32_t type, void *body, uint32_t size)
{
	if (key)
		put_key(d, key);
	switch (type) {
	case SPA_TYPE_Bool:
		put_value(d, NULL, *(int32_t*)body ? "true" : "false");
		break;
	case SPA_TYPE_Id:
	{
		const char *str;
		char fallback[32];
		uint32_t id = *(uint32_t*)body;
		str = spa_debug_type_find_short_name(info, *(uint32_t*)body);
		if (str == NULL) {
			snprintf(fallback, sizeof(fallback), "id-%08x", id);
			str = fallback;
		}
		put_value(d, NULL, str);
		break;
	}
	case SPA_TYPE_Int:
		put_int(d, NULL, *(int32_t*)body);
		break;
	case SPA_TYPE_Fd:
	case SPA_TYPE_Long:
		put_int(d, NULL, *(int64_t*)body);
		break;
	case SPA_TYPE_Float:
		put_double(d, NULL, *(float*)body);
		break;
	case SPA_TYPE_Double:
		put_double(d, NULL, *(double*)body);
		break;
	case SPA_TYPE_String:
		put_string(d, NULL, (const char*)body);
		break;
	case SPA_TYPE_Rectangle:
	{
                struct spa_rectangle *r = (struct spa_rectangle *)body;
		put_begin(d, NULL, "{", STATE_SIMPLE);
		put_int(d, "width", r->width);
		put_int(d, "height", r->height);
		put_end(d, "}", STATE_SIMPLE);
		break;
	}
	case SPA_TYPE_Fraction:
	{
                struct spa_fraction *f = (struct spa_fraction *)body;
		put_begin(d, NULL, "{", STATE_SIMPLE);
		put_int(d, "num", f->num);
		put_int(d, "denom", f->denom);
		put_end(d, "}", STATE_SIMPLE);
		break;
	}
	case SPA_TYPE_Array:
	{
		struct spa_pod_array_body *b = (struct spa_pod_array_body *)body;
		void *p;
		info = info && info->values ? info->values: info;
		put_begin(d, NULL, "[", STATE_SIMPLE);
		SPA_POD_ARRAY_BODY_FOREACH(b, size, p)
			put_pod_value(d, NULL, info, b->child.type, p, b->child.size);
		put_end(d, "]", STATE_SIMPLE);
		break;
	}
	case SPA_TYPE_Choice:
	{
		struct spa_pod_choice_body *b = (struct spa_pod_choice_body *)body;
		int index = 0;

		if (b->type == SPA_CHOICE_None) {
			put_pod_value(d, NULL, info, b->child.type,
					SPA_POD_CONTENTS(struct spa_pod, &b->child),
					b->child.size);
		} else {
			static const char * const range_labels[] = { "default", "min", "max", NULL };
			static const char * const step_labels[] = { "default", "min", "max", "step", NULL };
			static const char * const enum_labels[] = { "default", "alt%u" };
			static const char * const flags_labels[] = { "default", "flag%u" };

			const char * const *labels;
			const char *label;
			char buffer[64];
			int max_labels, flags = 0;
			void *p;

			switch (b->type) {
			case SPA_CHOICE_Range:
				labels = range_labels;
				max_labels = 3;
				flags |= STATE_SIMPLE;
				break;
			case SPA_CHOICE_Step:
				labels = step_labels;
				max_labels = 4;
				flags |= STATE_SIMPLE;
				break;
			case SPA_CHOICE_Enum:
				labels = enum_labels;
				max_labels = 1;
				break;
			case SPA_CHOICE_Flags:
				labels = flags_labels;
				max_labels = 1;
				break;
			default:
				labels = NULL;
				break;
			}
			if (labels == NULL)
				break;

			put_begin(d, NULL, "{", flags);
			SPA_POD_CHOICE_BODY_FOREACH(b, size, p) {
				if ((label = labels[SPA_CLAMP(index, 0, max_labels)]) == NULL)
					break;
				snprintf(buffer, sizeof(buffer), label, index);
				put_pod_value(d, buffer, info, b->child.type, p, b->child.size);
				index++;
			}
			put_end(d, "}", flags);
		}
		break;
	}
	case SPA_TYPE_Object:
        {
		put_begin(d, NULL, "{", 0);
		struct spa_pod_object_body *b = (struct spa_pod_object_body *)body;
		struct spa_pod_prop *p;
		const struct spa_type_info *ti, *ii;

		ti = spa_debug_type_find(info, b->type);
		ii = ti ? spa_debug_type_find(ti->values, 0) : NULL;
		ii = ii ? spa_debug_type_find(ii->values, b->id) : NULL;

		info = ti ? ti->values : info;

		SPA_POD_OBJECT_BODY_FOREACH(b, size, p) {
			char fallback[32];
			const char *name;

			ii = spa_debug_type_find(info, p->key);
			name = ii ? spa_debug_type_short_name(ii->name) : NULL;
			if (name == NULL) {
				snprintf(fallback, sizeof(fallback), "id-%08x", p->key);
				name = fallback;
			}
			put_pod_value(d, name,
					ii ? ii->values : NULL,
					p->value.type,
					SPA_POD_CONTENTS(struct spa_pod_prop, p),
					p->value.size);
		}
		put_end(d, "}", 0);
		break;
	}
	case SPA_TYPE_Struct:
	{
		struct spa_pod *b = (struct spa_pod *)body, *p;
		put_begin(d, NULL, "[", 0);
		SPA_POD_FOREACH(b, size, p)
			put_pod_value(d, NULL, info, p->type, SPA_POD_BODY(p), p->size);
		put_end(d, "]", 0);
		break;
	}
	case SPA_TYPE_None:
		put_value(d, NULL, NULL);
		break;
	}
}
static void put_pod(struct data *d, const char *key, const struct spa_pod *pod)
{
	if (pod == NULL) {
		put_value(d, key, NULL);
	} else {
		put_pod_value(d, key, SPA_TYPE_ROOT,
				SPA_POD_TYPE(pod),
				SPA_POD_BODY(pod),
				SPA_POD_BODY_SIZE(pod));
	}
}

static void put_params(struct data *d, const char *key,
		struct spa_param_info *params, uint32_t n_params,
		struct spa_list *list)
{
	uint32_t i;

	put_begin(d, key, "{", 0);
	for (i = 0; i < n_params; i++) {
		struct spa_param_info *pi = &params[i];
		struct param *p;
		uint32_t flags;

		flags = pi->flags & SPA_PARAM_INFO_READ ? 0 : STATE_SIMPLE;

		put_begin(d, spa_debug_type_find_short_name(spa_type_param, pi->id),
				"[", flags);
		spa_list_for_each(p, list, link) {
			if (p->id == pi->id)
				put_pod(d, NULL, p->param);
		}
		put_end(d, "]", flags);
	}
	put_end(d, "}", 0);
}

struct flags_info {
	const char *name;
	uint64_t mask;
};

static void put_flags(struct data *d, const char *key,
		uint64_t flags, const struct flags_info *info)
{
	uint32_t i;
	put_begin(d, key, "[", STATE_SIMPLE);
	for (i = 0; info[i].name != NULL; i++) {
		if (info[i].mask & flags)
			put_string(d, NULL, info[i].name);
	}
	put_end(d, "]", STATE_SIMPLE);
}

/* core */
static void core_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "props", PW_CORE_CHANGE_MASK_PROPS },
		{ NULL, 0 },
	};

	struct data *d = o->data;
	struct pw_core_info *i = d->info;

	put_begin(d, "info", "{", 0);
	put_int(d, "cookie", i->cookie);
	put_value(d, "user-name", i->user_name);
	put_value(d, "host-name", i->host_name);
	put_value(d, "version", i->version);
	put_value(d, "name", i->name);
	put_flags(d, "change-mask", i->change_mask, fl);
	put_dict(d, "props", i->props);
	put_end(d, "}", 0);
}

static const struct class core_class = {
	.type = PW_TYPE_INTERFACE_Core,
	.version = PW_VERSION_CORE,
	.dump = core_dump,
	.name_key = PW_KEY_CORE_NAME,
};

/* client */
static void client_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "props", PW_CLIENT_CHANGE_MASK_PROPS },
		{ NULL, 0 },
	};

	struct data *d = o->data;
	struct pw_client_info *i = o->info;

	put_begin(d, "info", "{", 0);
	put_flags(d, "change-mask", i->change_mask, fl);
	put_dict(d, "props", i->props);
	put_end(d, "}", 0);
}

static void client_event_info(void *data, const struct pw_client_info *info)
{
	struct object *o = data;
	int changed = 0;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->id, info->change_mask);

	info = o->info = pw_client_info_update(o->info, info);
	if (info == NULL)
		return;

	if (info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS)
		changed++;

	if (changed) {
		o->changed += changed;
		core_sync(o->data);
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
};

static void client_destroy(struct object *o)
{
	if (o->info) {
		pw_client_info_free(o->info);
		o->info = NULL;
	}
}

static const struct class client_class = {
	.type = PW_TYPE_INTERFACE_Client,
	.version = PW_VERSION_CLIENT,
	.events = &client_events,
	.destroy = client_destroy,
	.dump = client_dump,
	.name_key = PW_KEY_APP_NAME,
};

/* module */
static void module_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "props", PW_MODULE_CHANGE_MASK_PROPS },
		{ NULL, 0 },
	};

	struct data *d = o->data;
	struct pw_module_info *i = o->info;

	put_begin(d, "info", "{", 0);
	put_value(d, "name", i->name);
	put_value(d, "filename", i->filename);
	put_value(d, "args", i->args);
	put_flags(d, "change-mask", i->change_mask, fl);
	put_dict(d, "props", i->props);
	put_end(d, "}", 0);
}

static void module_event_info(void *data, const struct pw_module_info *info)
{
	struct object *o = data;
	int changed = 0;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->id, info->change_mask);

	info = o->info = pw_module_info_update(o->info, info);
	if (info == NULL)
		return;

	if (info->change_mask & PW_MODULE_CHANGE_MASK_PROPS)
		changed++;

	if (changed) {
		o->changed += changed;
		core_sync(o->data);
	}
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.info = module_event_info,
};

static void module_destroy(struct object *o)
{
	if (o->info) {
		pw_module_info_free(o->info);
		o->info = NULL;
	}
}

static const struct class module_class = {
	.type = PW_TYPE_INTERFACE_Module,
	.version = PW_VERSION_MODULE,
	.events = &module_events,
	.destroy = module_destroy,
	.dump = module_dump,
	.name_key = PW_KEY_MODULE_NAME,
};

/* factory */
static void factory_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "props", PW_FACTORY_CHANGE_MASK_PROPS },
		{ NULL, 0 },
	};

	struct data *d = o->data;
	struct pw_factory_info *i = o->info;

	put_begin(d, "info", "{", 0);
	put_value(d, "name", i->name);
	put_value(d, "type", i->type);
	put_int(d, "version", i->version);
	put_flags(d, "change-mask", i->change_mask, fl);
	put_dict(d, "props", i->props);
	put_end(d, "}", 0);
}

static void factory_event_info(void *data, const struct pw_factory_info *info)
{
	struct object *o = data;
	int changed = 0;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->id, info->change_mask);

	info = o->info = pw_factory_info_update(o->info, info);
	if (info == NULL)
		return;

	if (info->change_mask & PW_FACTORY_CHANGE_MASK_PROPS)
		changed++;

	if (changed) {
		o->changed += changed;
		core_sync(o->data);
	}
}

static const struct pw_factory_events factory_events = {
	PW_VERSION_FACTORY_EVENTS,
	.info = factory_event_info,
};

static void factory_destroy(struct object *o)
{
	if (o->info) {
		pw_factory_info_free(o->info);
		o->info = NULL;
	}
}

static const struct class factory_class = {
	.type = PW_TYPE_INTERFACE_Factory,
	.version = PW_VERSION_FACTORY,
	.events = &factory_events,
	.destroy = factory_destroy,
	.dump = factory_dump,
	.name_key = PW_KEY_FACTORY_NAME,
};

/* device */
static void device_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "props", PW_DEVICE_CHANGE_MASK_PROPS },
		{ "params", PW_DEVICE_CHANGE_MASK_PARAMS },
		{ NULL, 0 },
	};

	struct data *d = o->data;
	struct pw_device_info *i = o->info;

	put_begin(d, "info", "{", 0);
	put_flags(d, "change-mask", i->change_mask, fl);
	put_dict(d, "props", i->props);
	put_params(d, "params", i->params, i->n_params, &o->param_list);
	put_end(d, "}", 0);
}

static void device_event_info(void *data, const struct pw_device_info *info)
{
	struct object *o = data;
	uint32_t i, changed = 0;
	int res;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->id, info->change_mask);

	info = o->info = pw_device_info_update(o->info, info);
	if (info == NULL)
		return;

	o->params = info->params;
	o->n_params = info->n_params;

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS)
		changed++;

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t id = info->params[i].id;

			if (info->params[i].user == 0)
				continue;
			info->params[i].user = 0;

			changed++;
			add_param(&o->pending_list, 0, id, NULL);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;

			res = pw_device_enum_params((struct pw_device*)o->proxy,
					++info->params[i].seq, id, 0, -1, NULL);
			if (SPA_RESULT_IS_ASYNC(res))
				info->params[i].seq = res;
		}
	}
	if (changed) {
		o->changed += changed;
		core_sync(o->data);
	}
}

static void device_event_param(void *data, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct object *o = data;
	add_param(&o->pending_list, seq, id, param);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static void device_destroy(struct object *o)
{
	if (o->info) {
		pw_device_info_free(o->info);
		o->info = NULL;
	}
}

static const struct class device_class = {
	.type = PW_TYPE_INTERFACE_Device,
	.version = PW_VERSION_DEVICE,
	.events = &device_events,
	.destroy = device_destroy,
	.dump = device_dump,
	.name_key = PW_KEY_DEVICE_NAME,
};

/* node */
static void node_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "input-ports", PW_NODE_CHANGE_MASK_INPUT_PORTS },
		{ "output-ports", PW_NODE_CHANGE_MASK_OUTPUT_PORTS },
		{ "state", PW_NODE_CHANGE_MASK_STATE },
		{ "props", PW_NODE_CHANGE_MASK_PROPS },
		{ "params", PW_NODE_CHANGE_MASK_PARAMS },
		{ NULL, 0 },
	};

	struct data *d = o->data;
	struct pw_node_info *i = o->info;

	put_begin(d, "info", "{", 0);
	put_int(d, "max-input-ports", i->max_input_ports);
	put_int(d, "max-output-ports", i->max_output_ports);
	put_flags(d, "change-mask", i->change_mask, fl);
	put_int(d, "n-input-ports", i->n_input_ports);
	put_int(d, "n-output-ports", i->n_output_ports);
	put_value(d, "state", pw_node_state_as_string(i->state));
	put_value(d, "error", i->error);
	put_dict(d, "props", i->props);
	put_params(d, "params", i->params, i->n_params, &o->param_list);
	put_end(d, "}", 0);
}

static void node_event_info(void *data, const struct pw_node_info *info)
{
	struct object *o = data;
	uint32_t i, changed = 0;
	int res;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->id, info->change_mask);

	info = o->info = pw_node_info_update(o->info, info);
	if (info == NULL)
		return;

	o->params = info->params;
	o->n_params = info->n_params;

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
			add_param(&o->pending_list, 0, id, NULL);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;

			res = pw_node_enum_params((struct pw_node*)o->proxy,
					++info->params[i].seq, id, 0, -1, NULL);
			if (SPA_RESULT_IS_ASYNC(res))
				info->params[i].seq = res;
		}
	}
	if (changed) {
		o->changed += changed;
		core_sync(o->data);
	}
}

static void node_event_param(void *data, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct object *o = data;
	add_param(&o->pending_list, seq, id, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void node_destroy(struct object *o)
{
	if (o->info) {
		pw_node_info_free(o->info);
		o->info = NULL;
	}
}

static const struct class node_class = {
	.type = PW_TYPE_INTERFACE_Node,
	.version = PW_VERSION_NODE,
	.events = &node_events,
	.destroy = node_destroy,
	.dump = node_dump,
	.name_key = PW_KEY_NODE_NAME,
};

/* port */
static void port_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "props", PW_PORT_CHANGE_MASK_PROPS },
		{ "params", PW_PORT_CHANGE_MASK_PARAMS },
		{ NULL, },
	};

	struct data *d = o->data;
	struct pw_port_info *i = o->info;

	put_begin(d, "info", "{", 0);
	put_value(d, "direction", pw_direction_as_string(i->direction));
	put_flags(d, "change-mask", i->change_mask, fl);
	put_dict(d, "props", i->props);
	put_params(d, "params", i->params, i->n_params, &o->param_list);
	put_end(d, "}", 0);
}

static void port_event_info(void *data, const struct pw_port_info *info)
{
	struct object *o = data;
	uint32_t i, changed = 0;
	int res;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->id, info->change_mask);

	info = o->info = pw_port_info_update(o->info, info);
	if (info == NULL)
		return;

	o->params = info->params;
	o->n_params = info->n_params;

	if (info->change_mask & PW_PORT_CHANGE_MASK_PROPS)
		changed++;

	if (info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t id = info->params[i].id;

			if (info->params[i].user == 0)
				continue;
			info->params[i].user = 0;

			changed++;
			add_param(&o->pending_list, 0, id, NULL);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;

			res = pw_port_enum_params((struct pw_port*)o->proxy,
					++info->params[i].seq, id, 0, -1, NULL);
			if (SPA_RESULT_IS_ASYNC(res))
				info->params[i].seq = res;
		}
	}
	if (changed) {
		o->changed += changed;
		core_sync(o->data);
	}
}

static void port_event_param(void *data, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct object *o = data;
	add_param(&o->pending_list, seq, id, param);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_event_info,
	.param = port_event_param,
};

static void port_destroy(struct object *o)
{
	if (o->info) {
		pw_port_info_free(o->info);
		o->info = NULL;
	}
}

static const struct class port_class = {
	.type = PW_TYPE_INTERFACE_Port,
	.version = PW_VERSION_PORT,
	.events = &port_events,
	.destroy = port_destroy,
	.dump = port_dump,
	.name_key = PW_KEY_PORT_NAME,
};

/* link */
static void link_dump(struct object *o)
{
	static const struct flags_info fl[] = {
		{ "state", PW_LINK_CHANGE_MASK_STATE },
		{ "format", PW_LINK_CHANGE_MASK_FORMAT },
		{ "props", PW_LINK_CHANGE_MASK_PROPS },
		{ NULL, },
	};

	struct data *d = o->data;
	struct pw_link_info *i = o->info;

	put_begin(d, "info", "{", 0);
	put_int(d, "output-node-id", i->output_node_id);
	put_int(d, "output-port-id", i->output_port_id);
	put_int(d, "input-node-id", i->input_node_id);
	put_int(d, "input-port-id", i->input_port_id);
	put_flags(d, "change-mask", i->change_mask, fl);
	put_value(d, "state", pw_link_state_as_string(i->state));
	put_value(d, "error", i->error);
	put_pod(d, "format", i->format);
	put_dict(d, "props", i->props);
	put_end(d, "}", 0);
}

static void link_event_info(void *data, const struct pw_link_info *info)
{
	struct object *o = data;
	uint32_t changed = 0;

	pw_log_debug("object %p: id:%d change-mask:%08"PRIx64, o, o->id, info->change_mask);

	info = o->info = pw_link_info_update(o->info, info);
	if (info == NULL)
		return;

	if (info->change_mask & PW_LINK_CHANGE_MASK_STATE)
		changed++;

	if (info->change_mask & PW_LINK_CHANGE_MASK_FORMAT)
		changed++;

	if (info->change_mask & PW_LINK_CHANGE_MASK_PROPS)
		changed++;

	if (changed) {
		o->changed += changed;
		core_sync(o->data);
	}
}

static const struct pw_link_events link_events = {
	PW_VERSION_PORT_EVENTS,
	.info = link_event_info,
};

static void link_destroy(struct object *o)
{
	if (o->info) {
		pw_link_info_free(o->info);
		o->info = NULL;
	}
}

static const struct class link_class = {
	.type = PW_TYPE_INTERFACE_Link,
	.version = PW_VERSION_LINK,
	.events = &link_events,
	.destroy = link_destroy,
	.dump = link_dump,
};

static void json_dump_val(struct data *d, const char *key, struct spa_json *it, const char *value, int len)
{
	struct spa_json sub;
	if (spa_json_is_array(value, len)) {
		put_begin(d, key, "[", STATE_SIMPLE);
		spa_json_enter(it, &sub);
		while ((len = spa_json_next(&sub, &value)) > 0) {
			json_dump_val(d, NULL, &sub, value, len);
		}
		put_end(d, "]", STATE_SIMPLE);
	} else if (spa_json_is_object(value, len)) {
		char val[1024];
		put_begin(d, key, "{", STATE_SIMPLE);
		spa_json_enter(it, &sub);
		while (spa_json_get_string(&sub, val, sizeof(val)) > 0) {
			if ((len = spa_json_next(&sub, &value)) <= 0)
				break;
			json_dump_val(d, val, &sub, value, len);
		}
		put_end(d, "}", STATE_SIMPLE);
	} else if (spa_json_is_string(value, len)) {
		put_encoded_string(d, key, strndupa(value, len));
	} else {
		put_value(d, key, strndupa(value, len));
	}
}

static void json_dump(struct data *d, const char *key, const char *value)
{
	struct spa_json it[1];
	int len;
	const char *val;
	spa_json_init(&it[0], value, strlen(value));
	if ((len = spa_json_next(&it[0], &val)) >= 0)
		json_dump_val(d, key, &it[0], val, len);
}

/* metadata */

struct metadata_entry {
	struct spa_list link;
	uint32_t changed;
	uint32_t subject;
	char *key;
	char *value;
	char *type;
};

static void metadata_dump(struct object *o)
{
	struct data *d = o->data;
	struct metadata_entry *e;
	put_dict(d, "props", &o->props->dict);
	put_begin(d, "metadata", "[", 0);
	spa_list_for_each(e, &o->data_list, link) {
		if (e->changed == 0)
			continue;
		put_begin(d, NULL, "{", STATE_SIMPLE);
		put_int(d, "subject", e->subject);
		put_value(d, "key", e->key);
		put_value(d, "type", e->type);
		if (e->type != NULL && spa_streq(e->type, "Spa:String:JSON"))
			json_dump(d, "value", e->value);
		else
			put_value(d, "value", e->value);
		put_end(d, "}", STATE_SIMPLE);
		e->changed = 0;
	}
	put_end(d, "]", 0);
}

static struct metadata_entry *metadata_find(struct object *o, uint32_t subject, const char *key)
{
	struct metadata_entry *e;
	spa_list_for_each(e, &o->data_list, link) {
		if ((e->subject == subject) &&
		    (key == NULL || spa_streq(e->key, key)))
			return e;
	}
	return NULL;
}

static int metadata_property(void *data,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
	struct object *o = data;
	struct metadata_entry *e;

	while ((e = metadata_find(o, subject, key)) != NULL) {
		spa_list_remove(&e->link);
		free(e);
	}
	if (key != NULL && value != NULL) {
		size_t size = strlen(key) + 1;
		size += strlen(value) + 1;
		size += type ? strlen(type) + 1 : 0;

		e = calloc(1, sizeof(*e) + size);
		if (e == NULL)
			return -errno;

		e->subject = subject;
		e->key = SPA_PTROFF(e, sizeof(*e), void);
		strcpy(e->key, key);
		e->value = SPA_PTROFF(e->key, strlen(e->key) + 1, void);
		strcpy(e->value, value);
		if (type) {
			e->type = SPA_PTROFF(e->value, strlen(e->value) + 1, void);
			strcpy(e->type, type);
		} else {
			e->type = NULL;
		}
		spa_list_append(&o->data_list, &e->link);
		e->changed++;
	}
	o->changed++;
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void metadata_destroy(struct object *o)
{
	struct metadata_entry *e;
	spa_list_consume(e, &o->data_list, link) {
		spa_list_remove(&e->link);
		free(e);
	}
}

static const struct class metadata_class = {
	.type = PW_TYPE_INTERFACE_Metadata,
	.version = PW_VERSION_METADATA,
	.events = &metadata_events,
	.destroy = metadata_destroy,
	.dump = metadata_dump,
	.name_key = PW_KEY_METADATA_NAME,
};

static const struct class *classes[] =
{
	&core_class,
	&module_class,
	&factory_class,
	&client_class,
	&device_class,
	&node_class,
	&port_class,
	&link_class,
	&metadata_class,
};

static const struct class *find_class(const char *type, uint32_t version)
{
	SPA_FOR_EACH_ELEMENT_VAR(classes, c) {
		if (spa_streq((*c)->type, type) &&
		    (*c)->version <= version)
			return *c;
	}
	return NULL;
}

static void
destroy_removed(void *data)
{
	struct object *o = data;
	pw_proxy_destroy(o->proxy);
}

static void
destroy_proxy(void *data)
{
	struct object *o = data;

	spa_hook_remove(&o->proxy_listener);
	if (o->class != NULL) {
		if (o->class->events)
			spa_hook_remove(&o->object_listener);
		if (o->class->destroy)
	                o->class->destroy(o);
	}
        o->proxy = NULL;
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
	struct data *d = data;
	struct object *o;

	o = calloc(1, sizeof(*o));
	if (o == NULL) {
		pw_log_error("can't alloc object for %u %s/%d: %m", id, type, version);
		return;
	}
	o->data = d;
	o->id = id;
	o->permissions = permissions;
	o->type = strdup(type);
	o->version = version;
	o->props = props ? pw_properties_new_dict(props) : NULL;
	spa_list_init(&o->param_list);
	spa_list_init(&o->pending_list);
	spa_list_init(&o->data_list);

	o->class = find_class(type, version);
	if (o->class != NULL) {
		o->proxy = pw_registry_bind(d->registry,
				id, type, o->class->version, 0);
		if (o->proxy == NULL)
			goto bind_failed;

		pw_proxy_add_listener(o->proxy,
				&o->proxy_listener,
				&proxy_events, o);

		if (o->class->events)
			pw_proxy_add_object_listener(o->proxy,
					&o->object_listener,
					o->class->events, o);
		else
			o->changed++;
	} else {
		o->changed++;
	}
	spa_list_append(&d->object_list, &o->link);

	core_sync(d);
	return;

bind_failed:
	pw_log_error("can't bind object for %u %s/%d: %m", id, type, version);
	pw_properties_free(o->props);
	free(o);
	return;
}

static bool object_matches(struct object *o, const char *pattern)
{
	uint32_t id;
	const char *str;

	if (spa_atou32(pattern, &id, 0) && o->id == id)
		return true;

	if (o->props == NULL)
		return false;

	if (strstr(o->type, pattern) != NULL)
		return true;
	if ((str = pw_properties_get(o->props, PW_KEY_OBJECT_PATH)) != NULL &&
	    fnmatch(pattern, str, FNM_EXTMATCH) == 0)
		return true;
	if ((str = pw_properties_get(o->props, PW_KEY_OBJECT_SERIAL)) != NULL &&
	    spa_streq(pattern, str))
		return true;
	if (o->class != NULL && o->class->name_key != NULL &&
	    (str = pw_properties_get(o->props, o->class->name_key)) != NULL &&
	    fnmatch(pattern, str, FNM_EXTMATCH) == 0)
		return true;
	return false;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct data *d = data;
	struct object *o;

	if ((o = find_object(d, id)) == NULL)
		return;

	d->state = STATE_FIRST;
	if (d->pattern != NULL && !object_matches(o, d->pattern))
		return;
	if (d->state == STATE_FIRST)
		put_begin(d, NULL, "[", 0);
	put_begin(d, NULL, "{", 0);
	put_int(d, "id", o->id);
	if (o->class && o->class->dump)
		put_value(d, "info", NULL);
	else if (o->props)
		put_value(d, "props", NULL);
	put_end(d, "}", 0);
	if (d->state != STATE_FIRST)
		put_end(d, "]\n", 0);

	object_destroy(o);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void dump_objects(struct data *d)
{
	static const struct flags_info fl[] = {
		{ "r", PW_PERM_R },
		{ "w", PW_PERM_W },
		{ "x", PW_PERM_X },
		{ "m", PW_PERM_M },
		{ "l", PW_PERM_L },
		{ NULL, },
	};

	struct object *o;

	d->state = STATE_FIRST;
	spa_list_for_each(o, &d->object_list, link) {
		if (d->pattern != NULL && !object_matches(o, d->pattern))
			continue;
		if (o->changed == 0)
			continue;
		if (d->state == STATE_FIRST)
			put_begin(d, NULL, "[", 0);
		put_begin(d, NULL, "{", 0);
		put_int(d, "id", o->id);
		put_value(d, "type", o->type);
		put_int(d, "version", o->version);
		put_flags(d, "permissions", o->permissions, fl);
		if (o->class && o->class->dump)
			o->class->dump(o);
		else if (o->props)
			put_dict(d, "props", &o->props->dict);
		put_end(d, "}", 0);
		o->changed = 0;
	}
	if (d->state != STATE_FIRST)
		put_end(d, "]\n", 0);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct data *d = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(d->loop);
}

static void on_core_info(void *data, const struct pw_core_info *info)
{
	struct data *d = data;
	d->info = pw_core_info_update(d->info, info);
}

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;
	struct object *o;

	if (id == PW_ID_CORE) {
		if (d->sync_seq != seq)
			return;

		pw_log_debug("sync end %u/%u", d->sync_seq, seq);

		spa_list_for_each(o, &d->object_list, link)
			object_update_params(&o->param_list, &o->pending_list,
					o->n_params, o->params);

		dump_objects(d);
		if (!d->monitor)
			pw_main_loop_quit(d->loop);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.info = on_core_info,
	.error = on_core_error,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void show_help(struct data *data, const char *name, bool error)
{
	fprintf(error ? stderr : stdout, "%s [options] [<id>]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n"
		"  -m, --monitor                         monitor changes\n"
		"  -N, --no-colors                       disable color output\n"
		"  -C, --color[=WHEN]                    whether to enable color support. WHEN is `never`, `always`, or `auto`\n",
		name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct object *o;
	struct pw_loop *l;
	const char *opt_remote = NULL;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ "monitor",	no_argument,		NULL, 'm' },
		{ "no-colors",	no_argument,		NULL, 'N' },
		{ "color",	optional_argument,	NULL, 'C' },
		{ NULL, 0, NULL, 0}
	};
	int c;

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	data.out = stdout;
	if (getenv("NO_COLOR") == NULL && isatty(fileno(data.out)))
		colors = true;
	setlinebuf(data.out);

	while ((c = getopt_long(argc, argv, "hVr:mNC", long_options, NULL)) != -1) {
		switch (c) {
		case 'h' :
			show_help(&data, argv[0], false);
			return 0;
		case 'V' :
			printf("%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r' :
			opt_remote = optarg;
			break;
		case 'm' :
			data.monitor = true;
			break;
		case 'N' :
			colors = false;
			break;
		case 'C' :
			if (optarg == NULL || !strcmp(optarg, "auto"))
				break; /* nothing to do, tty detection was done
					  before parsing options */
			else if (!strcmp(optarg, "never"))
				colors = false;
			else if (!strcmp(optarg, "always"))
				colors = true;
			else {
				fprintf(stderr, "Unknown color: %s\n", optarg);
				show_help(&data, argv[0], true);
				return -1;
			}
			break;
		default:
			show_help(&data, argv[0], true);
			return -1;
		}
	}

	if (optind < argc)
		data.pattern = argv[optind++];

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "can't create main loop: %m\n");
		return -1;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "can't create context: %m\n");
		return -1;
	}

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, opt_remote ? opt_remote :
					("[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]"),
				NULL),
			0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect: %m\n");
		return -1;
	}

	spa_list_init(&data.object_list);

	pw_core_add_listener(data.core,
			&data.core_listener,
			&core_events, &data);
	data.registry = pw_core_get_registry(data.core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(data.registry,
			&data.registry_listener,
			&registry_events, &data);

	pw_main_loop_run(data.loop);

	spa_list_consume(o, &data.object_list, link)
		object_destroy(o);
	if (data.info)
		pw_core_info_free(data.info);

	spa_hook_remove(&data.registry_listener);
	pw_proxy_destroy((struct pw_proxy*)data.registry);
	spa_hook_remove(&data.core_listener);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return 0;
}
