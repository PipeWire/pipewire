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
#include <pipewire/array.h>

#include <lilv/lilv.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/buf-size/buf-size.h>

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
	char *p;
	size_t i = 0;

	pw_array_for_each(p, &table->array) {
		if (spa_streq(p, uri))
			return i + 1;
		i++;
	}

	pw_array_add_ptr(&table->array, strdup(uri));
	return pw_array_get_len(&table->array, char*);
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

	URITable uri_table;
	LV2_URID_Map map;
	LV2_URID_Unmap unmap;
	LV2_Feature map_feature;
	LV2_Feature unmap_feature;
	const LV2_Feature *features[5];
};

static void context_free(struct context *c)
{
	if (c->world) {
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

static struct context *context_new(void)
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

	c->map.handle = &c->uri_table;
	c->map.map = uri_table_map;
	c->map_feature.URI = LV2_URID_MAP_URI;
	c->map_feature.data = &c->map;
	c->unmap.handle = &c->uri_table;
	c->unmap.unmap  = uri_table_unmap;
	c->unmap_feature.URI = LV2_URID_UNMAP_URI;
	c->unmap_feature.data = &c->unmap;
	c->features[0] = &c->map_feature;
	c->features[1] = &c->unmap_feature;
	c->features[2] = &buf_size_features[0];
	c->features[3] = &buf_size_features[1];
	c->features[4] = &buf_size_features[2];

	return c;
error:
	context_free(c);
	return NULL;
}

static struct context *context_ref(void)
{
	if (_context == NULL) {
		_context = context_new();
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

static void *lv2_instantiate(const struct fc_descriptor *desc,
                        unsigned long *SampleRate, int index, const char *config)
{
	struct descriptor *d = (struct descriptor*)desc;
	struct plugin *p = d->p;
	struct context *c = p->c;
	return lilv_plugin_instantiate(p->p, *SampleRate, c->features);
}

static void lv2_cleanup(void *instance)
{
	lilv_instance_free(instance);
}

static void lv2_connect_port(void *instance, unsigned long port, float *data)
{
	lilv_instance_connect_port(instance, port, data);
}

static void lv2_activate(void *instance)
{
	lilv_instance_activate(instance);
}

static void lv2_deactivate(void *instance)
{
	lilv_instance_deactivate(instance);
}

static void lv2_run(void *instance, unsigned long SampleCount)
{
	lilv_instance_run(instance, SampleCount);
}

static void lv2_free(struct fc_descriptor *desc)
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

struct fc_plugin *load_lv2_plugin(const char *plugin_uri, const char *config)
{
	struct context *c;
	const LilvPlugins *plugins;
	const LilvPlugin *plugin;
	LilvNode *uri;
	int res;
	struct plugin *p;

	c = context_ref();
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
