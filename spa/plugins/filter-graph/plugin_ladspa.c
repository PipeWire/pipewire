/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <math.h>
#include <limits.h>

#include <spa/utils/result.h>
#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/support/log.h>

#include "audio-plugin.h"
#include "ladspa.h"

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_log *log;

	void *hndl;
	LADSPA_Descriptor_Function desc_func;
};

struct descriptor {
	struct spa_fga_descriptor desc;
	const LADSPA_Descriptor *d;
};

static void *ladspa_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor *desc,
                        unsigned long SampleRate, int index, const char *config)
{
	struct descriptor *d = (struct descriptor *)desc;
	return d->d->instantiate(d->d, SampleRate);
}

static const LADSPA_Descriptor *find_desc(LADSPA_Descriptor_Function desc_func, const char *name)
{
	unsigned long i;
	for (i = 0; ;i++) {
		const LADSPA_Descriptor *d = desc_func(i);
		if (d == NULL)
			break;
		if (spa_streq(d->Label, name))
			return d;
	}
	return NULL;
}

static float get_default(struct spa_fga_port *port, LADSPA_PortRangeHintDescriptor hint,
		LADSPA_Data lower, LADSPA_Data upper)
{
	LADSPA_Data def;

	switch (hint & LADSPA_HINT_DEFAULT_MASK) {
	case LADSPA_HINT_DEFAULT_MINIMUM:
		def = lower;
		break;
	case LADSPA_HINT_DEFAULT_MAXIMUM:
		def = upper;
		break;
	case LADSPA_HINT_DEFAULT_LOW:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) expf(logf(lower) * 0.75f + logf(upper) * 0.25f);
		else
			def = (LADSPA_Data) (lower * 0.75f + upper * 0.25f);
		break;
	case LADSPA_HINT_DEFAULT_MIDDLE:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) expf(logf(lower) * 0.5f + logf(upper) * 0.5f);
		else
			def = (LADSPA_Data) (lower * 0.5f + upper * 0.5f);
		break;
	case LADSPA_HINT_DEFAULT_HIGH:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) expf(logf(lower) * 0.25f + logf(upper) * 0.75f);
		else
			def = (LADSPA_Data) (lower * 0.25f + upper * 0.75f);
		break;
	case LADSPA_HINT_DEFAULT_0:
		def = 0.0f;
		break;
	case LADSPA_HINT_DEFAULT_1:
		def = 1.0f;
		break;
	case LADSPA_HINT_DEFAULT_100:
		def = 100.0f;
		break;
	case LADSPA_HINT_DEFAULT_440:
		def = 440.0f;
		break;
	default:
		if (upper == lower)
			def = upper;
		else
			def = SPA_CLAMPF(0.5f * upper, lower, upper);
		break;
	}
	if (LADSPA_IS_HINT_INTEGER(hint))
		def = roundf(def);
	return def;
}

static void ladspa_port_update_ranges(struct descriptor *dd, struct spa_fga_port *port)
{
	const LADSPA_Descriptor *d = dd->d;
	unsigned long p = port->index;
	LADSPA_PortRangeHintDescriptor hint = d->PortRangeHints[p].HintDescriptor;
	LADSPA_Data lower, upper;

	lower = d->PortRangeHints[p].LowerBound;
	upper = d->PortRangeHints[p].UpperBound;

	port->hint = 0;
	if (hint & LADSPA_HINT_TOGGLED)
		port->hint |= SPA_FGA_HINT_BOOLEAN;
	if (hint & LADSPA_HINT_SAMPLE_RATE)
		port->hint |= SPA_FGA_HINT_SAMPLE_RATE;
	if (hint & LADSPA_HINT_INTEGER)
		port->hint |= SPA_FGA_HINT_INTEGER;
	if (spa_streq(port->name, "latency"))
		port->hint |= SPA_FGA_HINT_LATENCY;
	port->def = get_default(port, hint, lower, upper);
	port->min = lower;
	port->max = upper;
}

static void ladspa_free(const struct spa_fga_descriptor *desc)
{
	struct descriptor *d = (struct descriptor*)desc;
	free(d->desc.ports);
	free(d);
}

static const struct spa_fga_descriptor *ladspa_plugin_make_desc(void *plugin, const char *name)
{
	struct plugin *p = (struct plugin *)plugin;
	struct descriptor *desc;
	const LADSPA_Descriptor *d;
	uint32_t i;

	d = find_desc(p->desc_func, name);
	if (d == NULL)
		return NULL;

	desc = calloc(1, sizeof(*desc));
	desc->d = d;

	desc->desc.instantiate = ladspa_instantiate;
	desc->desc.cleanup = d->cleanup;
	desc->desc.connect_port = (__typeof__(desc->desc.connect_port))d->connect_port;
	desc->desc.activate = d->activate;
	desc->desc.deactivate = d->deactivate;
	desc->desc.run = d->run;

	desc->desc.free = ladspa_free;

	desc->desc.name = d->Label;
	desc->desc.flags = 0;

	desc->desc.n_ports = d->PortCount;
	desc->desc.ports = calloc(desc->desc.n_ports, sizeof(struct spa_fga_port));

	for (i = 0; i < desc->desc.n_ports; i++) {
		desc->desc.ports[i].index = i;
		desc->desc.ports[i].name = d->PortNames[i];
		desc->desc.ports[i].flags = d->PortDescriptors[i];
		ladspa_port_update_ranges(desc, &desc->desc.ports[i]);
	}
	return &desc->desc;
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = ladspa_plugin_make_desc,
};

static int ladspa_handle_load_by_path(struct plugin *impl, const char *path)
{
	int res;
	void *handle = NULL;
	LADSPA_Descriptor_Function desc_func;

	handle = dlopen(path, RTLD_NOW);
	if (handle == NULL) {
		spa_log_debug(impl->log, "failed to open '%s': %s", path, dlerror());
		res = -ENOENT;
		goto exit;
	}

	spa_log_info(impl->log, "successfully opened '%s'", path);

	desc_func = (LADSPA_Descriptor_Function) dlsym(handle, "ladspa_descriptor");
	if (desc_func == NULL) {
		spa_log_warn(impl->log, "cannot find descriptor function in '%s': %s", path, dlerror());
		res = -ENOSYS;
		goto exit;
	}

	impl->hndl = handle;
	impl->desc_func = desc_func;
	return 0;

exit:
	if (handle)
		dlclose(handle);
	return res;
}

static inline const char *split_walk(const char *str, const char *delimiter, size_t * len, const char **state)
{
	const char *s = *state ? *state : str;

	s += strspn(s, delimiter);
	if (*s == '\0')
		return NULL;

	*len = strcspn(s, delimiter);
	*state = s + *len;

	return s;
}

static int load_ladspa_plugin(struct plugin *impl, const char *path)
{
	int res = -ENOENT;

	if (path[0] != '/') {
		const char *search_dirs, *p, *state = NULL;
		char filename[PATH_MAX];
		size_t len;

		search_dirs = getenv("LADSPA_PATH");
		if (!search_dirs)
			search_dirs = "/usr/lib64/ladspa:/usr/lib/ladspa:" LIBDIR;

		/*
		 * set the errno for the case when `ladspa_handle_load_by_path()`
		 * is never called, which can only happen if the supplied
		 * LADSPA_PATH contains too long paths
		 */
		res = -ENAMETOOLONG;

		while ((p = split_walk(search_dirs, ":", &len, &state))) {
			int namelen;

			if (len >= sizeof(filename))
				continue;

			namelen = snprintf(filename, sizeof(filename), "%.*s/%s.so", (int) len, p, path);
			if (namelen < 0 || (size_t) namelen >= sizeof(filename))
				continue;

			res = ladspa_handle_load_by_path(impl, filename);
			if (res >= 0)
				break;
		}
	}
	else {
		res = ladspa_handle_load_by_path(impl, path);
	}
	return res;
}

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct plugin *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct plugin *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin))
		*interface = &impl->plugin;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct plugin *impl = (struct plugin *)handle;
	if (impl->hndl)
		dlclose(impl->hndl);
	impl->hndl = NULL;
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct plugin);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct plugin *impl;
	uint32_t i;
	int res;
	const char *path = NULL;

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct plugin *) handle;

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "filter.graph.path"))
			path = s;
	}
	if (path == NULL)
		return -EINVAL;

	if ((res = load_ladspa_plugin(impl, path)) < 0) {
		spa_log_error(impl->log, "failed to load plugin '%s': %s",
				path, spa_strerror(res));
		return res;
	}

	impl->plugin.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,
			SPA_VERSION_FGA_PLUGIN,
			&impl_plugin, impl);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin },
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static struct spa_handle_factory spa_fga_plugin_ladspa_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.ladspa",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_fga_plugin_ladspa_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
