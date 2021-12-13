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

#include <spa/support/plugin.h>
#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>

struct fc_plugin {
	const struct fc_descriptor *(*make_desc)(struct fc_plugin *plugin, const char *name);
	void (*unload) (struct fc_plugin *plugin);
};

struct fc_port {
	uint32_t index;
	const char *name;
#define FC_PORT_INPUT	(1ULL << 0)
#define FC_PORT_OUTPUT	(1ULL << 1)
#define FC_PORT_CONTROL	(1ULL << 2)
#define FC_PORT_AUDIO	(1ULL << 3)
	uint64_t flags;

#define FC_HINT_SAMPLE_RATE	(1ULL << 3)
	uint64_t hint;
	float def;
	float min;
	float max;
};

#define FC_IS_PORT_INPUT(x)	((x) & FC_PORT_INPUT)
#define FC_IS_PORT_OUTPUT(x)	((x) & FC_PORT_OUTPUT)
#define FC_IS_PORT_CONTROL(x)	((x) & FC_PORT_CONTROL)
#define FC_IS_PORT_AUDIO(x)	((x) & FC_PORT_AUDIO)

struct fc_descriptor {
	const char *name;
#define FC_DESCRIPTOR_SUPPORTS_NULL_DATA	(1ULL << 0)
	uint64_t flags;

	void (*free) (struct fc_descriptor *desc);

	uint32_t n_ports;
	struct fc_port *ports;

	void *(*instantiate) (const struct fc_descriptor *desc,
			unsigned long *SampleRate, int index, const char *config);

	void (*cleanup) (void *instance);

	void (*connect_port) (void *instance, unsigned long port, float *data);

	void (*activate) (void *instance);
	void (*deactivate) (void *instance);

	void (*run) (void *instance, unsigned long SampleCount);
};

static inline void fc_plugin_free(struct fc_plugin *plugin)
{
	if (plugin->unload)
		plugin->unload(plugin);
}

static inline void fc_descriptor_free(struct fc_descriptor *desc)
{
	if (desc->free)
		desc->free(desc);
}

struct fc_plugin *load_ladspa_plugin(const struct spa_support *support, uint32_t n_support,
		const char *path, const char *config);
struct fc_plugin *load_lv2_plugin(const struct spa_support *support, uint32_t n_support,
		const char *path, const char *config);
struct fc_plugin *load_builtin_plugin(const struct spa_support *support, uint32_t n_support,
		const char *path, const char *config);
