/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PLUGIN_H
#define PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#include <spa/support/plugin.h>

#include "dsp-ops.h"

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

#define FC_HINT_BOOLEAN		(1ULL << 2)
#define FC_HINT_SAMPLE_RATE	(1ULL << 3)
#define FC_HINT_INTEGER		(1ULL << 5)
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
#define FC_DESCRIPTOR_COPY			(1ULL << 1)
	uint64_t flags;

	void (*free) (const struct fc_descriptor *desc);

	uint32_t n_ports;
	struct fc_port *ports;

	void *(*instantiate) (const struct fc_descriptor *desc,
			unsigned long SampleRate, int index, const char *config);

	void (*cleanup) (void *instance);

	void (*connect_port) (void *instance, unsigned long port, float *data);
	void (*control_changed) (void *instance);

	void (*activate) (void *instance);
	void (*deactivate) (void *instance);

	void (*run) (void *instance, unsigned long SampleCount);
};

static inline void fc_plugin_free(struct fc_plugin *plugin)
{
	if (plugin->unload)
		plugin->unload(plugin);
}

static inline void fc_descriptor_free(const struct fc_descriptor *desc)
{
	if (desc->free)
		desc->free(desc);
}

#define FC_PLUGIN_LOAD_FUNC "pipewire__filter_chain_plugin_load"

typedef struct fc_plugin *(fc_plugin_load_func)(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *dsp, const char *path, const char *config);


#endif /* PLUGIN_H */
