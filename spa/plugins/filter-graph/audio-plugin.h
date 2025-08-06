/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_FGA_PLUGIN_H
#define SPA_FGA_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/support/plugin.h>

#define SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin	SPA_TYPE_INFO_INTERFACE_BASE "FilterGraph:AudioPlugin"

#define SPA_VERSION_FGA_PLUGIN	0
struct spa_fga_plugin { struct spa_interface iface; };

struct spa_fga_plugin_methods {
#define SPA_VERSION_FGA_PLUGIN_METHODS		0
	uint32_t version;

	const struct spa_fga_descriptor *(*make_desc) (void *plugin, const char *name);
};

struct spa_fga_port {
	uint32_t index;
	const char *name;
#define SPA_FGA_PORT_INPUT		(1ULL << 0)
#define SPA_FGA_PORT_OUTPUT		(1ULL << 1)
#define SPA_FGA_PORT_CONTROL		(1ULL << 2)
#define SPA_FGA_PORT_AUDIO		(1ULL << 3)
#define SPA_FGA_PORT_SUPPORTS_NULL_DATA (1ULL << 4)
	uint64_t flags;

#define SPA_FGA_HINT_BOOLEAN		(1ULL << 0)
#define SPA_FGA_HINT_SAMPLE_RATE	(1ULL << 1)
#define SPA_FGA_HINT_INTEGER		(1ULL << 2)
#define SPA_FGA_HINT_LATENCY		(1ULL << 3)
	uint64_t hint;
	float def;
	float min;
	float max;
};

#define SPA_FGA_IS_PORT_INPUT(x)	((x) & SPA_FGA_PORT_INPUT)
#define SPA_FGA_IS_PORT_OUTPUT(x)	((x) & SPA_FGA_PORT_OUTPUT)
#define SPA_FGA_IS_PORT_CONTROL(x)	((x) & SPA_FGA_PORT_CONTROL)
#define SPA_FGA_IS_PORT_AUDIO(x)	((x) & SPA_FGA_PORT_AUDIO)
#define SPA_FGA_SUPPORTS_NULL_DATA(x)	((x) & SPA_FGA_PORT_SUPPORTS_NULL_DATA)

struct spa_fga_descriptor {
	const char *name;
#define SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA	(1ULL << 0)
#define SPA_FGA_DESCRIPTOR_COPY			(1ULL << 1)
	uint64_t flags;

	void (*free) (const struct spa_fga_descriptor *desc);

	uint32_t n_ports;
	struct spa_fga_port *ports;

	void *(*instantiate) (const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor *desc,
			unsigned long SampleRate, int index, const char *config);

	void (*cleanup) (void *instance);

	void (*connect_port) (void *instance, unsigned long port, float *data);
	void (*control_changed) (void *instance);

	void (*activate) (void *instance);
	void (*deactivate) (void *instance);

	void (*run) (void *instance, unsigned long SampleCount);
};

static inline void spa_fga_descriptor_free(const struct spa_fga_descriptor *desc)
{
	if (desc->free)
		desc->free(desc);
}

static inline const struct spa_fga_descriptor *
spa_fga_plugin_make_desc(struct spa_fga_plugin *plugin, const char *name)
{
	return spa_api_method_r(const struct spa_fga_descriptor *, NULL,
			spa_fga_plugin, &plugin->iface, make_desc, 0, name);
}

typedef struct spa_fga_plugin *(spa_filter_graph_audio_plugin_load_func_t)(const struct spa_support *support,
		uint32_t n_support, const char *path, const struct spa_dict *info);

#define SPA_FILTER_GRAPH_AUDIO_PLUGIN_LOAD_FUNC_NAME "spa_filter_graph_audio_plugin_load"



#endif /* PLUGIN_H */
