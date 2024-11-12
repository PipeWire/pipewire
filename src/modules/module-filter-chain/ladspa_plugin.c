/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <math.h>
#include <limits.h>

#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/support/log.h>

#include "audio-plugin.h"
#include "ladspa.h"

struct plugin {
	struct spa_fga_plugin plugin;

	struct spa_log *log;

	void *handle;
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

	port->hint = hint;
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
	desc->desc.connect_port = d->connect_port;
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

static void ladspa_plugin_free(void *plugin)
{
	struct plugin *p = (struct plugin *)plugin;
	if (p->handle)
		dlclose(p->handle);
	free(p);
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = ladspa_plugin_make_desc,
	.free = ladspa_plugin_free
};

static struct spa_fga_plugin *ladspa_handle_load_by_path(struct spa_log *log, const char *path)
{
	struct plugin *p;
	int res;
	void *handle = NULL;
	LADSPA_Descriptor_Function desc_func;

	handle = dlopen(path, RTLD_NOW);
	if (handle == NULL) {
		spa_log_debug(log, "failed to open '%s': %s", path, dlerror());
		res = -ENOENT;
		goto exit;
	}

	spa_log_info(log, "successfully opened '%s'", path);

	desc_func = (LADSPA_Descriptor_Function) dlsym(handle, "ladspa_descriptor");
	if (desc_func == NULL) {
		spa_log_warn(log, "cannot find descriptor function in '%s': %s", path, dlerror());
		res = -ENOSYS;
		goto exit;
	}

	p = calloc(1, sizeof(*p));
	if (!p) {
		res = -errno;
		goto exit;
	}
	p->log = log;
	p->handle = handle;
	p->desc_func = desc_func;

	p->plugin.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,
			SPA_VERSION_FGA_PLUGIN,
			&impl_plugin, p);

	return &p->plugin;

exit:
	if (handle)
		dlclose(handle);
	errno = -res;
	return NULL;
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

struct spa_fga_plugin *load_ladspa_plugin(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *dsp, const char *plugin, const struct spa_dict *info)
{
	struct spa_fga_plugin *pl = NULL;
	struct spa_log *log;

	log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	if (plugin[0] != '/') {
		const char *search_dirs, *p, *state = NULL;
		char path[PATH_MAX];
		size_t len;

		search_dirs = getenv("LADSPA_PATH");
		if (!search_dirs)
			search_dirs = "/usr/lib64/ladspa:/usr/lib/ladspa:" LIBDIR;

		/*
		 * set the errno for the case when `ladspa_handle_load_by_path()`
		 * is never called, which can only happen if the supplied
		 * LADSPA_PATH contains too long paths
		 */
		errno = ENAMETOOLONG;

		while ((p = split_walk(search_dirs, ":", &len, &state))) {
			int pathlen;

			if (len >= sizeof(path))
				continue;

			pathlen = snprintf(path, sizeof(path), "%.*s/%s.so", (int) len, p, plugin);
			if (pathlen < 0 || (size_t) pathlen >= sizeof(path))
				continue;

			pl = ladspa_handle_load_by_path(log, path);
			if (pl != NULL)
				break;
		}
	}
	else {
		pl = ladspa_handle_load_by_path(log, plugin);
	}

	if (pl == NULL)
		spa_log_error(log, "failed to load plugin '%s': %s", plugin, strerror(errno));

	return pl;
}
