/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <gio/gio.h>
#include <glib.h>

#include <spa/debug/mem.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread.h>

#include "../module.h"

#define NAME "gsettings"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define PA_GSETTINGS_MODULE_GROUP_SCHEMA "org.freedesktop.pulseaudio.module-group"
#define PA_GSETTINGS_MODULE_GROUPS_SCHEMA "org.freedesktop.pulseaudio.module-groups"
#define PA_GSETTINGS_MODULE_GROUPS_PATH "/org/freedesktop/pulseaudio/module-groups/"

#define MAX_MODULES	10

struct module_gsettings_data {
	struct module *module;

	GMainContext *context;
	GMainLoop *loop;
	struct spa_thread *thr;

	GSettings *settings;
	gchar **group_names;

	struct spa_list groups;
};

struct group {
	struct spa_list link;
	char *name;
	struct module *module;
	struct spa_hook module_listener;
};

struct info {
	bool enabled;
	char *name;
	char *module[MAX_MODULES];
	char *args[MAX_MODULES];
};

static void clean_info(const struct info *info)
{
	int i;
	for (i = 0; i < MAX_MODULES; i++) {
		g_free(info->module[i]);
		g_free(info->args[i]);
	}
	g_free(info->name);
}

static void unload_module(struct module_gsettings_data *d, struct group *g)
{
	spa_list_remove(&g->link);
	g_free(g->name);
	if (g->module)
		module_unload(g->module);
	free(g);
}

static void unload_group(struct module_gsettings_data *d, const char *name)
{
	struct group *g, *t;
	spa_list_for_each_safe(g, t, &d->groups, link) {
		if (spa_streq(g->name, name))
			unload_module(d, g);
	}
}
static void module_destroy(void *data)
{
	struct group *g = data;
	if (g->module) {
		spa_hook_remove(&g->module_listener);
		g->module = NULL;
	}
}

static const struct module_events module_gsettings_events = {
	VERSION_MODULE_EVENTS,
	.destroy = module_destroy
};

static int load_group(struct module_gsettings_data *d, const struct info *info)
{
	struct group *g;
	int i, res;

	for (i = 0; i < MAX_MODULES; i++) {
		if (info->module[i] == NULL || strlen(info->module[i]) <= 0)
			break;

		g = calloc(1, sizeof(struct group));
		if (g == NULL)
			return -errno;

		g->name = strdup(info->name);
		g->module = module_create(d->module->impl, info->module[i], info->args[i]);
		if (g->module == NULL) {
			pw_log_info("can't create module:%s args:%s: %m",
					info->module[i], info->args[i]);
		} else {
			module_add_listener(g->module, &g->module_listener,
					&module_gsettings_events, g);
			if ((res = module_load(g->module)) < 0) {
				pw_log_warn("can't load module:%s args:%s: %s",
						info->module[i], info->args[i],
						spa_strerror(res));
			}
		}
		spa_list_append(&d->groups, &g->link);
	}
	return 0;
}

static int
do_handle_info(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct module_gsettings_data *d = user_data;
	const struct info *info = data;

	unload_group(d, info->name);
	if (info->enabled)
		load_group(d, info);

	clean_info(info);
	return 0;
}

static bool schema_exists(const char *schema_id)
{
	GSettingsSchemaSource *source;
	GSettingsSchema *schema;

	source = g_settings_schema_source_get_default();
	if (!source) {
		pw_log_error("gsettings schema source not found");
		return false;
	}

	schema = g_settings_schema_source_lookup(source, schema_id, TRUE);
	if (!schema) {
		pw_log_error("required gsettings schema %s does not exist", schema_id);
		return false;
	}

	g_settings_schema_unref(schema);
	return true;
}

static void handle_module_group(struct module_gsettings_data *d, gchar *name)
{
	struct impl *impl = d->module->impl;
	GSettings *settings;
	gchar p[1024];
	struct info info;
	int i;

	snprintf(p, sizeof(p), PA_GSETTINGS_MODULE_GROUPS_PATH"%s/", name);

	if (!schema_exists(PA_GSETTINGS_MODULE_GROUP_SCHEMA))
		return;

	settings = g_settings_new_with_path(PA_GSETTINGS_MODULE_GROUP_SCHEMA, p);
	if (settings == NULL)
		return;

	spa_zero(info);
	info.name = strdup(p);
	info.enabled = g_settings_get_boolean(settings, "enabled");

	for (i = 0; i < MAX_MODULES; i++) {
		snprintf(p, sizeof(p), "name%d", i);
		info.module[i] = g_settings_get_string(settings, p);

		snprintf(p, sizeof(p), "args%i", i);
		info.args[i] = g_settings_get_string(settings, p);
	}
	pw_loop_invoke(impl->loop, do_handle_info, 0,
			&info, sizeof(info), false, d);

	g_object_unref(G_OBJECT(settings));
}

static void module_group_callback(GSettings *settings, gchar *key, gpointer user_data)
{
	struct module_gsettings_data *d = g_object_get_data(G_OBJECT(settings), "module-data");
	handle_module_group(d, user_data);
}

static void *do_loop(void *user_data)
{
	struct module_gsettings_data *d = user_data;

	pw_log_info("enter");
	g_main_context_push_thread_default(d->context);

	d->loop = g_main_loop_new(d->context, FALSE);

	g_main_loop_run(d->loop);

	g_main_context_pop_thread_default(d->context);
	g_main_loop_unref (d->loop);
	d->loop = NULL;
	pw_log_info("leave");

	return NULL;
}

static int module_gsettings_load(struct module *module)
{
	struct module_gsettings_data *data = module->user_data;
	gchar **name;

	/* Check the required schema files are installed. If not, Glib will
	 * abort in g_settings_new */
	if (!schema_exists(PA_GSETTINGS_MODULE_GROUPS_SCHEMA) ||
			!schema_exists(PA_GSETTINGS_MODULE_GROUP_SCHEMA))
		return -EIO;

	data->context = g_main_context_new();
	g_main_context_push_thread_default(data->context);

	data->settings = g_settings_new(PA_GSETTINGS_MODULE_GROUPS_SCHEMA);
	if (data->settings == NULL) {
		g_main_context_pop_thread_default(data->context);
		return -EIO;
	}

	data->group_names = g_settings_list_children(data->settings);

	for (name = data->group_names; *name; name++) {
		GSettings *child = g_settings_get_child(data->settings, *name);
		/* The child may have been removed between the
		 * g_settings_list_children() and g_settings_get_child() calls. */
		if (child == NULL)
			continue;

		g_object_set_data(G_OBJECT(child), "module-data", data);
		g_signal_connect(child, "changed", (GCallback) module_group_callback, *name);
		handle_module_group(data, *name);
	}
	g_main_context_pop_thread_default(data->context);

	data->thr = pw_thread_utils_create(NULL, do_loop, data);
	return 0;
}

static gboolean
do_stop(gpointer data)
{
	struct module_gsettings_data *d = data;
	if (d->loop)
		g_main_loop_quit(d->loop);
	return FALSE;
}

static int module_gsettings_unload(struct module *module)
{
	struct module_gsettings_data *d = module->user_data;
	struct group *g;

	if (d->context) {
		g_main_context_invoke(d->context, do_stop, d);
		if (d->thr)
			pw_thread_utils_join(d->thr, NULL);
		g_main_context_unref(d->context);
	}

	spa_list_consume(g, &d->groups, link)
		unload_module(d, g);

	g_strfreev(d->group_names);
	if (d->settings)
		g_object_unref(G_OBJECT(d->settings));
	return 0;
}

static int module_gsettings_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct module_gsettings_data * const data = module->user_data;
	spa_list_init(&data->groups);
	data->module = module;

	return 0;
}

static const struct spa_dict_item module_gsettings_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "GSettings Adapter" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

DEFINE_MODULE_INFO(module_gsettings) = {
	.name = "module-gsettings",
	.load_once = true,
	.prepare = module_gsettings_prepare,
	.load = module_gsettings_load,
	.unload = module_gsettings_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_gsettings_info),
	.data_size = sizeof(struct module_gsettings_data),
};
