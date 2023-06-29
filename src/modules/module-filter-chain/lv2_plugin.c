/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <math.h>

#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/support/loop.h>

#include <pipewire/log.h>
#include <pipewire/utils.h>
#include <pipewire/array.h>

#include <lilv/lilv.h>

#if defined __has_include
#	if __has_include (<lv2/atom/atom.h>)

		#include <lv2/atom/atom.h>
		#include <lv2/buf-size/buf-size.h>
		#include <lv2/worker/worker.h>
		#include <lv2/options/options.h>
		#include <lv2/parameters/parameters.h>

#	else

		#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
		#include <lv2/lv2plug.in/ns/ext/buf-size/buf-size.h>
		#include <lv2/lv2plug.in/ns/ext/worker/worker.h>
		#include <lv2/lv2plug.in/ns/ext/options/options.h>
		#include <lv2/lv2plug.in/ns/ext/parameters/parameters.h>

#	endif

#endif

#include "plugin.h"

static struct context *_context;

typedef struct URITable {
	struct pw_array array;
} URITable;

static void uri_table_init(URITable *table)
{
	pw_array_init(&table->array, 1024);
}

static void uri_table_destroy(URITable *table)
{
	char **p;
	pw_array_for_each(p, &table->array)
		free(*p);
	pw_array_clear(&table->array);
}

static LV2_URID uri_table_map(LV2_URID_Map_Handle handle, const char *uri)
{
	URITable *table = (URITable*)handle;
	char **p;
	size_t i = 0;

	pw_array_for_each(p, &table->array) {
		i++;
		if (spa_streq(*p, uri))
			goto done;
	}
	pw_array_add_ptr(&table->array, strdup(uri));
	i =  pw_array_get_len(&table->array, char*);
done:
	return i;
}

static const char *uri_table_unmap(LV2_URID_Map_Handle handle, LV2_URID urid)
{
	URITable *table = (URITable*)handle;

	if (urid > 0 && urid <= pw_array_get_len(&table->array, char*))
		return *pw_array_get_unchecked(&table->array, urid, char*);
	return NULL;
}

struct context {
	int ref;
	LilvWorld *world;

	struct spa_loop *data_loop;
	struct spa_loop *main_loop;

	LilvNode *lv2_InputPort;
	LilvNode *lv2_OutputPort;
	LilvNode *lv2_AudioPort;
	LilvNode *lv2_ControlPort;
	LilvNode *lv2_Optional;
	LilvNode *atom_AtomPort;
	LilvNode *atom_Sequence;
	LilvNode *urid_map;
	LilvNode *powerOf2BlockLength;
	LilvNode *fixedBlockLength;
	LilvNode *boundedBlockLength;
	LilvNode* worker_schedule;
	LilvNode* worker_iface;

	URITable uri_table;
	LV2_URID_Map map;
	LV2_Feature map_feature;
	LV2_URID_Unmap unmap;
	LV2_Feature unmap_feature;

	LV2_URID atom_Int;
	LV2_URID atom_Float;
};

#define context_map(c,uri) ((c)->map.map((c)->map.handle,(uri)))

static void context_free(struct context *c)
{
	if (c->world) {
		lilv_node_free(c->worker_schedule);
		lilv_node_free(c->powerOf2BlockLength);
		lilv_node_free(c->fixedBlockLength);
		lilv_node_free(c->boundedBlockLength);
		lilv_node_free(c->urid_map);
		lilv_node_free(c->atom_Sequence);
		lilv_node_free(c->atom_AtomPort);
		lilv_node_free(c->lv2_Optional);
		lilv_node_free(c->lv2_ControlPort);
		lilv_node_free(c->lv2_AudioPort);
		lilv_node_free(c->lv2_OutputPort);
		lilv_node_free(c->lv2_InputPort);
		lilv_world_free(c->world);
	}
	uri_table_destroy(&c->uri_table);
	free(c);
}

static const LV2_Feature buf_size_features[3] = {
	{ LV2_BUF_SIZE__powerOf2BlockLength, NULL },
	{ LV2_BUF_SIZE__fixedBlockLength,    NULL },
	{ LV2_BUF_SIZE__boundedBlockLength,  NULL },
};

static struct context *context_new(const struct spa_support *support, uint32_t n_support)
{
	struct context *c;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;

	uri_table_init(&c->uri_table);
	c->world = lilv_world_new();
	if (c->world == NULL)
		goto error;

	lilv_world_load_all(c->world);

	c->lv2_InputPort = lilv_new_uri(c->world, LV2_CORE__InputPort);
	c->lv2_OutputPort = lilv_new_uri(c->world, LV2_CORE__OutputPort);
	c->lv2_AudioPort = lilv_new_uri(c->world, LV2_CORE__AudioPort);
	c->lv2_ControlPort = lilv_new_uri(c->world, LV2_CORE__ControlPort);
	c->lv2_Optional = lilv_new_uri(c->world, LV2_CORE__connectionOptional);
	c->atom_AtomPort = lilv_new_uri(c->world, LV2_ATOM__AtomPort);
	c->atom_Sequence = lilv_new_uri(c->world, LV2_ATOM__Sequence);
	c->urid_map = lilv_new_uri(c->world, LV2_URID__map);
	c->powerOf2BlockLength = lilv_new_uri(c->world, LV2_BUF_SIZE__powerOf2BlockLength);
	c->fixedBlockLength = lilv_new_uri(c->world, LV2_BUF_SIZE__fixedBlockLength);
	c->boundedBlockLength = lilv_new_uri(c->world, LV2_BUF_SIZE__boundedBlockLength);
        c->worker_schedule = lilv_new_uri(c->world, LV2_WORKER__schedule);
	c->worker_iface = lilv_new_uri(c->world, LV2_WORKER__interface);

	c->map.handle = &c->uri_table;
	c->map.map = uri_table_map;
	c->map_feature.URI = LV2_URID_MAP_URI;
	c->map_feature.data = &c->map;
	c->unmap.handle = &c->uri_table;
	c->unmap.unmap  = uri_table_unmap;
	c->unmap_feature.URI = LV2_URID_UNMAP_URI;
	c->unmap_feature.data = &c->unmap;

	c->atom_Int = context_map(c, LV2_ATOM__Int);
	c->atom_Float = context_map(c, LV2_ATOM__Float);

	c->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	c->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);

	return c;
error:
	context_free(c);
	return NULL;
}

static struct context *context_ref(const struct spa_support *support, uint32_t n_support)
{
	if (_context == NULL) {
		_context = context_new(support, n_support);
		if (_context == NULL)
			return NULL;
	}
	_context->ref++;
	return _context;
}

static void context_unref(struct context *context)
{
	if (--_context->ref == 0) {
		context_free(_context);
		_context = NULL;
	}
}

struct plugin {
	struct fc_plugin plugin;
	struct context *c;
	const LilvPlugin *p;
};

struct descriptor {
	struct fc_descriptor desc;
	struct plugin *p;
};

struct instance {
	struct descriptor *desc;
	LilvInstance *instance;
	LV2_Worker_Schedule work_schedule;
	LV2_Feature work_schedule_feature;
	LV2_Options_Option options[6];
	LV2_Feature options_feature;

	const LV2_Feature *features[7];

	const LV2_Worker_Interface *work_iface;

	int32_t block_length;
};

static int
do_respond(struct spa_loop *loop, bool async, uint32_t seq, const void *data,
		size_t size, void *user_data)
{
	struct instance *i = (struct instance*)user_data;
	i->work_iface->work_response(i->instance->lv2_handle, size, data);
	return 0;
}

/** Called by the plugin to respond to non-RT work. */
static LV2_Worker_Status
work_respond(LV2_Worker_Respond_Handle handle, uint32_t size, const void *data)
{
	struct instance *i = (struct instance*)handle;
	struct context *c = i->desc->p->c;
	spa_loop_invoke(c->data_loop, do_respond, 1, data, size, false, i);
	return LV2_WORKER_SUCCESS;
}

static int
do_schedule(struct spa_loop *loop, bool async, uint32_t seq, const void *data,
		size_t size, void *user_data)
{
	struct instance *i = (struct instance*)user_data;
	i->work_iface->work(i->instance->lv2_handle, work_respond, i, size, data);
	return 0;
}

/** Called by the plugin to schedule non-RT work. */
static LV2_Worker_Status
work_schedule(LV2_Worker_Schedule_Handle handle, uint32_t size, const void *data)
{
	struct instance *i = (struct instance*)handle;
	struct context *c = i->desc->p->c;
	spa_loop_invoke(c->main_loop, do_schedule, 1, data, size, false, i);
	return LV2_WORKER_SUCCESS;
}

static void *lv2_instantiate(const struct fc_descriptor *desc,
                        unsigned long SampleRate, int index, const char *config)
{
	struct descriptor *d = (struct descriptor*)desc;
	struct plugin *p = d->p;
	struct context *c = p->c;
	struct instance *i;
	uint32_t n_features = 0;
	static const int32_t min_block_length = 1;
	static const int32_t max_block_length = 8192;
	static const int32_t seq_size = 32768;
	float fsample_rate = SampleRate;

	i = calloc(1, sizeof(*i));
	if (i == NULL)
		return NULL;

	i->block_length = 1024;
	i->desc = d;
	i->features[n_features++] = &c->map_feature;
	i->features[n_features++] = &c->unmap_feature;
	i->features[n_features++] = &buf_size_features[0];
	i->features[n_features++] = &buf_size_features[1];
	i->features[n_features++] = &buf_size_features[2];
	if (lilv_plugin_has_feature(p->p, c->worker_schedule)) {
		i->work_schedule.handle = i;
		i->work_schedule.schedule_work = work_schedule;
		i->work_schedule_feature.URI = LV2_WORKER__schedule;
		i->work_schedule_feature.data = &i->work_schedule;
		i->features[n_features++] = &i->work_schedule_feature;
	}

	i->options[0] = (LV2_Options_Option) { LV2_OPTIONS_INSTANCE, 0,
		context_map(c, LV2_BUF_SIZE__minBlockLength), sizeof(int32_t),
		c->atom_Int, &min_block_length };
	i->options[1] = (LV2_Options_Option) { LV2_OPTIONS_INSTANCE, 0,
		context_map(c, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t),
		c->atom_Int, &max_block_length };
	i->options[2] = (LV2_Options_Option) { LV2_OPTIONS_INSTANCE, 0,
		context_map(c, LV2_BUF_SIZE__sequenceSize), sizeof(int32_t),
		c->atom_Int, &seq_size };
	i->options[3] = (LV2_Options_Option) { LV2_OPTIONS_INSTANCE, 0,
		context_map(c, "http://lv2plug.in/ns/ext/buf-size#nominalBlockLength"), sizeof(int32_t),
		c->atom_Int, &i->block_length },
	i->options[4] = (LV2_Options_Option) { LV2_OPTIONS_INSTANCE, 0,
		context_map(c, LV2_PARAMETERS__sampleRate), sizeof(float),
		c->atom_Float, &fsample_rate };
	i->options[5] = (LV2_Options_Option) { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL };

        i->options_feature.URI = LV2_OPTIONS__options;
        i->options_feature.data = i->options;
        i->features[n_features++] = &i->options_feature;

	i->instance = lilv_plugin_instantiate(p->p, SampleRate, i->features);
	if (i->instance == NULL) {
		free(i);
		return NULL;
	}
	if (lilv_plugin_has_extension_data(p->p, c->worker_iface)) {
                i->work_iface = (const LV2_Worker_Interface*)
			lilv_instance_get_extension_data(i->instance, LV2_WORKER__interface);
        }

	return i;
}

static void lv2_cleanup(void *instance)
{
	struct instance *i = instance;
	lilv_instance_free(i->instance);
	free(i);
}

static void lv2_connect_port(void *instance, unsigned long port, float *data)
{
	struct instance *i = instance;
	lilv_instance_connect_port(i->instance, port, data);
}

static void lv2_activate(void *instance)
{
	struct instance *i = instance;
	lilv_instance_activate(i->instance);
}

static void lv2_deactivate(void *instance)
{
	struct instance *i = instance;
	lilv_instance_deactivate(i->instance);
}

static void lv2_run(void *instance, unsigned long SampleCount)
{
	struct instance *i = instance;
	lilv_instance_run(i->instance, SampleCount);
	if (i->work_iface != NULL && i->work_iface->end_run != NULL)
		i->work_iface->end_run(i->instance);
}

static void lv2_free(const struct fc_descriptor *desc)
{
	struct descriptor *d = (struct descriptor*)desc;
	free((char*)d->desc.name);
	free(d->desc.ports);
	free(d);
}

static const struct fc_descriptor *lv2_make_desc(struct fc_plugin *plugin, const char *name)
{
	struct plugin *p = (struct plugin *)plugin;
	struct context *c = p->c;
	struct descriptor *desc;
	uint32_t i;
	float *mins, *maxes, *controls;

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL)
		return NULL;

	desc->p = p;
	desc->desc.instantiate = lv2_instantiate;
	desc->desc.cleanup = lv2_cleanup;
	desc->desc.connect_port = lv2_connect_port;
	desc->desc.activate = lv2_activate;
	desc->desc.deactivate = lv2_deactivate;
	desc->desc.run = lv2_run;

	desc->desc.free = lv2_free;

	desc->desc.name = strdup(name);
	desc->desc.flags = 0;

	desc->desc.n_ports = lilv_plugin_get_num_ports(p->p);
	desc->desc.ports = calloc(desc->desc.n_ports, sizeof(struct fc_port));

	mins = alloca(desc->desc.n_ports * sizeof(float));
	maxes = alloca(desc->desc.n_ports * sizeof(float));
	controls = alloca(desc->desc.n_ports * sizeof(float));

	lilv_plugin_get_port_ranges_float(p->p, mins, maxes, controls);

	for (i = 0; i < desc->desc.n_ports; i++) {
		const LilvPort *port = lilv_plugin_get_port_by_index(p->p, i);
                const LilvNode *symbol = lilv_port_get_symbol(p->p, port);
		struct fc_port *fp = &desc->desc.ports[i];

		fp->index = i;
		fp->name = strdup(lilv_node_as_string(symbol));

		fp->flags = 0;
		if (lilv_port_is_a(p->p, port, c->lv2_InputPort))
			fp->flags |= FC_PORT_INPUT;
		if (lilv_port_is_a(p->p, port, c->lv2_OutputPort))
			fp->flags |= FC_PORT_OUTPUT;
		if (lilv_port_is_a(p->p, port, c->lv2_ControlPort))
			fp->flags |= FC_PORT_CONTROL;
		if (lilv_port_is_a(p->p, port, c->lv2_AudioPort))
			fp->flags |= FC_PORT_AUDIO;

		fp->hint = 0;
		fp->min = mins[i];
		fp->max = maxes[i];
		fp->def = controls[i];
	}
	return &desc->desc;
}

static void lv2_unload(struct fc_plugin *plugin)
{
	struct plugin *p = (struct plugin *)plugin;
	context_unref(p->c);
	free(p);
}

SPA_EXPORT
struct fc_plugin *pipewire__filter_chain_plugin_load(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *ops, const char *plugin_uri, const char *config)
{
	struct context *c;
	const LilvPlugins *plugins;
	const LilvPlugin *plugin;
	LilvNode *uri;
	int res;
	struct plugin *p;

	c = context_ref(support, n_support);
	if (c == NULL)
		return NULL;

	uri = lilv_new_uri(c->world, plugin_uri);
	if (uri == NULL) {
		pw_log_warn("invalid URI %s", plugin_uri);
		res = -EINVAL;
		goto error_unref;
	}

	plugins = lilv_world_get_all_plugins(c->world);
	plugin = lilv_plugins_get_by_uri(plugins, uri);
	lilv_node_free(uri);

	if (plugin == NULL) {
		pw_log_warn("can't load plugin %s", plugin_uri);
		res = -EINVAL;
		goto error_unref;
	}

	p = calloc(1, sizeof(*p));
	if (!p) {
		res = -errno;
		goto error_unref;
	}
	p->p = plugin;
	p->c = c;

	p->plugin.make_desc = lv2_make_desc;
	p->plugin.unload = lv2_unload;

	return &p->plugin;

error_unref:
	context_unref(c);
	errno = -res;
	return NULL;
}
