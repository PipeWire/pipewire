/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <dlfcn.h>
#include <math.h>

#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>

#include <pipewire/log.h>
#include <pipewire/utils.h>

#include "plugin.h"
#include "ladspa.h"


struct plugin_data {
	void *handle;
	LADSPA_Descriptor_Function desc_func;
};

struct desc_data {
	const LADSPA_Descriptor *d;
};

static void *ladspa_instantiate(const struct fc_descriptor *desc,
                        unsigned long SampleRate, const char *config)
{
	struct desc_data *dd = desc->user;
	return dd->d->instantiate(dd->d, SampleRate);
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

static void ladspa_free(struct fc_descriptor *desc)
{
	free(desc->ports);
}

static const char *ladspa_get_prop(struct fc_descriptor *desc, const char *name)
{
	return NULL;
}

static float get_default(struct fc_port *port, LADSPA_PortRangeHintDescriptor hint,
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
			def = (LADSPA_Data) exp(log(lower) * 0.75 + log(upper) * 0.25);
		else
			def = (LADSPA_Data) (lower * 0.75 + upper * 0.25);
		break;
	case LADSPA_HINT_DEFAULT_MIDDLE:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) exp(log(lower) * 0.5 + log(upper) * 0.5);
		else
			def = (LADSPA_Data) (lower * 0.5 + upper * 0.5);
		break;
	case LADSPA_HINT_DEFAULT_HIGH:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) exp(log(lower) * 0.25 + log(upper) * 0.75);
		else
			def = (LADSPA_Data) (lower * 0.25 + upper * 0.75);
		break;
	case LADSPA_HINT_DEFAULT_0:
		def = 0;
		break;
	case LADSPA_HINT_DEFAULT_1:
		def = 1;
		break;
	case LADSPA_HINT_DEFAULT_100:
		def = 100;
		break;
	case LADSPA_HINT_DEFAULT_440:
		def = 440;
		break;
	default:
		if (upper == lower)
			def = upper;
		else
			def = SPA_CLAMP(0.5 * upper, lower, upper);
		break;
	}
	if (LADSPA_IS_HINT_INTEGER(hint))
		def = roundf(def);
	return def;
}

static float ladspa_port_get_param(struct fc_port *port, const char *name)
{
	struct fc_descriptor *desc = port->desc;
	struct desc_data *dd = desc->user;
	const LADSPA_Descriptor *d = dd->d;
	unsigned long p = port->index;
	LADSPA_PortRangeHintDescriptor hint = d->PortRangeHints[p].HintDescriptor;
	LADSPA_Data lower, upper;

	lower = d->PortRangeHints[p].LowerBound;
	upper = d->PortRangeHints[p].UpperBound;

	if (LADSPA_IS_HINT_SAMPLE_RATE(hint)) {
		lower *= (LADSPA_Data) 48000.0f;
		upper *= (LADSPA_Data) 48000.0f;
	}

	if (spa_streq(name, "default")) {
		return get_default(port, hint, lower, upper);
	}
	if (spa_streq(name, "min")) {
		return lower;
	}
	if (spa_streq(name, "max")) {
		return upper;
	}
	return 0.0f;
}

static const struct fc_descriptor *ladspa_make_desc(struct fc_plugin *plugin, const char *name)
{
	struct plugin_data *pd = plugin->user;
	struct fc_descriptor *desc;
	struct desc_data *dd;
	const LADSPA_Descriptor *d;
	uint32_t i;

	d = find_desc(pd->desc_func, name);
	if (d == NULL)
		return NULL;

	desc = fc_descriptor_new(plugin, sizeof(*dd));
	dd = desc->user;
	dd->d = d;

	desc->instantiate = ladspa_instantiate;
	desc->cleanup = d->cleanup;
	desc->connect_port = d->connect_port;
	desc->activate = d->activate;
	desc->deactivate = d->deactivate;
	desc->run = d->run;

	desc->free = ladspa_free;
	desc->get_prop = ladspa_get_prop;

	desc->name = d->Label;
	desc->flags = d->Properties;

	desc->n_ports = d->PortCount;
	desc->ports = calloc(desc->n_ports, sizeof(struct fc_port));

	for (i = 0; i < desc->n_ports; i++) {
		desc->ports[i].index = i;
		desc->ports[i].name = d->PortNames[i];
		desc->ports[i].flags = d->PortDescriptors[i];
		desc->ports[i].get_param = ladspa_port_get_param;
	}
	return desc;
}

static void ladspa_unload(struct fc_plugin *plugin)
{
	struct plugin_data *pd = plugin->user;
	if (pd->handle)
		dlclose(pd->handle);
}

static struct fc_plugin *ladspa_handle_load_by_path(const char *path)
{
	struct fc_plugin *plugin;
	struct plugin_data *pd;
	int res;

	plugin = fc_plugin_new(sizeof(*pd));
	if (!plugin)
		return NULL;

	pd = plugin->user;

	pd->handle = dlopen(path, RTLD_NOW);
	if (!pd->handle) {
		pw_log_debug("failed to open '%s': %s", path, dlerror());
		res = -ENOENT;
		goto exit;
	}

	pw_log_info("successfully opened '%s'", path);

	pd->desc_func = (LADSPA_Descriptor_Function) dlsym(pd->handle, "ladspa_descriptor");
	if (!pd->desc_func) {
		pw_log_warn("cannot find descriptor function in '%s': %s", path, dlerror());
		res = -ENOSYS;
		goto exit;
	}
	plugin->make_desc = ladspa_make_desc;
	plugin->unload = ladspa_unload;

	return plugin;

exit:
	if (pd->handle)
		dlclose(pd->handle);
	errno = -res;
	return NULL;
}

struct fc_plugin *load_ladspa_plugin(const char *plugin, const char *config)
{
	struct fc_plugin *pl = NULL;

	if (plugin[0] != '/') {
		const char *search_dirs, *p;
		char path[PATH_MAX];
		size_t len;

		search_dirs = getenv("LADSPA_PATH");
		if (!search_dirs)
			search_dirs = "/usr/lib64/ladspa";

		/*
		 * set the errno for the case when `ladspa_handle_load_by_path()`
		 * is never called, which can only happen if the supplied
		 * LADSPA_PATH contains too long paths
		 */
		errno = ENAMETOOLONG;

		while ((p = pw_split_walk(NULL, ":", &len, &search_dirs))) {
			int pathlen;

			if (len >= sizeof(path))
				continue;

			pathlen = snprintf(path, sizeof(path), "%.*s/%s.so", (int) len, p, plugin);
			if (pathlen < 0 || (size_t) pathlen >= sizeof(path))
				continue;

			pl = ladspa_handle_load_by_path(path);
			if (pl != NULL)
				break;
		}
	}
	else {
		pl = ladspa_handle_load_by_path(plugin);
	}

	if (pl == NULL)
		pw_log_error("failed to load plugin '%s': %s", plugin, strerror(errno));

	return pl;
}
