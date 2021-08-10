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

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>

#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>

struct fc_plugin {
	void *user;

	const struct fc_descriptor *(*make_desc)(struct fc_plugin *plugin, const char *name);
	void (*unload) (struct fc_plugin *plugin);
};

struct fc_port {
	uint32_t index;
	struct fc_descriptor *desc;
	const char *name;
	uint64_t flags;

	float (*get_param) (struct fc_port *port, const char *name);
};

struct fc_descriptor {
	struct fc_plugin *plugin;
	void *user;

	const char *name;
	uint64_t flags;

	void (*free) (struct fc_descriptor *desc);

	const char *(*get_prop) (struct fc_descriptor *desc, const char *name);

	uint32_t n_ports;
	struct fc_port *ports;

	void *(*instantiate) (const struct fc_descriptor *desc,
			unsigned long SampleRate, const char *config);

	void (*cleanup) (void *instance);

	void (*connect_port) (void *instance, unsigned long port, float *data);

	void (*activate) (void *instance);
	void (*deactivate) (void *instance);

	void (*run) (void *instance, unsigned long SampleCount);
};

static inline struct fc_plugin *fc_plugin_new(size_t extra)
{
	struct fc_plugin *plugin;

	plugin = calloc(1, sizeof(*plugin) + extra);
	plugin->user = SPA_PTROFF(plugin, sizeof(*plugin), void);
	return plugin;
}

static inline void fc_plugin_free(struct fc_plugin *plugin)
{
	if (plugin->unload)
		plugin->unload(plugin);
	free(plugin);
}

static inline struct fc_descriptor *fc_descriptor_new(struct fc_plugin *plugin, size_t extra)
{
	struct fc_descriptor *desc;

	desc = calloc(1, sizeof(*desc) + extra);
	desc->plugin = plugin;
	desc->user = SPA_PTROFF(desc, sizeof(*desc), void);
	return desc;
}

static inline void fc_descriptor_free(struct fc_descriptor *desc)
{
	if (desc->free)
		desc->free(desc);
	free(desc);
}

struct fc_plugin *load_ladspa_plugin(const char *path, const char *config);
struct fc_plugin *load_builtin_plugin(const char *path, const char *config);
