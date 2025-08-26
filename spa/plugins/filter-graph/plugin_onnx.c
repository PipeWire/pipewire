/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <math.h>
#include <limits.h>
#include <string.h>

#include <spa/utils/result.h>
#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/support/log.h>

#include <onnxruntime/onnxruntime_c_api.h>

#include "audio-plugin.h"

#define MAX_PORTS	256
#define MAX_CTX		64

const OrtApi* ort = NULL;

#define CHECK(expr)							\
	if ((status = (expr)) != NULL)					\
		goto error_onnx;

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_log *log;

	OrtEnv *env;
	OrtAllocator *allocator;
	OrtSessionOptions *session_options;
};

struct tensor_info {
	int index;
	enum spa_direction direction;
	char *name;
	enum ONNXTensorElementDataType type;
	int64_t dimensions[64];
	size_t n_dimensions;
	int retain;
#define DATA_NONE		0
#define DATA_PORT		1
#define DATA_CONTROL		2
#define DATA_PARAM_RATE		3
#define DATA_TENSOR		4
	uint32_t data_type;
	char data_name[128];
	uint32_t data_index;
	uint32_t data_size;
};

struct descriptor {
	struct spa_fga_descriptor desc;
	struct plugin *p;

	int blocksize;
	OrtSession *session;
	struct tensor_info tensors[MAX_PORTS];
	size_t n_tensors;
};

struct instance {
	struct descriptor *desc;

	uint32_t rate;

	OrtRunOptions *run_options;
	OrtValue *tensor[MAX_PORTS];

	uint32_t offset;
	float *data[MAX_PORTS];
};

static struct tensor_info *find_tensor(struct descriptor *d, const char *name, enum spa_direction direction)
{
	size_t i;
	for (i = 0; i < d->n_tensors; i++) {
		struct tensor_info *ti = &d->tensors[i];
		if (spa_streq(ti->name, name) && ti->direction == direction)
			return ti;
	}
	return NULL;
}

/*
 * {
 *   dimensions = [ 1, 576 ]
 *   retain = 64
 *   data = "tensor:<name>"|"param:rate"|"port:<port-name>"|"control:<port-name>"
 * }
 */
static int parse_tensor_info(struct descriptor *d, struct spa_json *it,
		struct tensor_info *info)
{
	struct plugin *p = d->p;
	struct spa_json sub;
	const char *val;
	int len;
	char key[256];
	char data[512];

	while ((len = spa_json_object_next(it, key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "dimensions")) {
			int64_t dimensions[64];
			size_t i, n_dimensions = 0;
			if (!spa_json_is_array(val, len)) {
				spa_log_error(p->log, "onnx: %s expects an array", key);
				return -EINVAL;
			}
			spa_json_enter(it, &sub);
			while (spa_json_get_string(&sub, data, sizeof(data)) > 0 && n_dimensions < 64)
				dimensions[n_dimensions++] = atoi(data);

			if (info->n_dimensions == 0)
				info->n_dimensions = n_dimensions;
			else if (n_dimensions != info->n_dimensions) {
				spa_log_error(p->log, "onnx: %s expected %zu dimensions, got %zu",
						key, info->n_dimensions, n_dimensions);
				return -EINVAL;
			}
			for (i = 0; i < n_dimensions; i++) {
				if (info->dimensions[i] <= 0)
					info->dimensions[i] = dimensions[i];
				else if (info->dimensions[i] != dimensions[i]) {
					spa_log_error(p->log, "onnx: %s mismatched %zu dimension, got %"
							PRIi64" expected %"PRIi64,
							key, i, dimensions[i], info->dimensions[i]);
					return -EINVAL;
				}
			}
		} else if (spa_streq(key, "retain")) {
			if (spa_json_parse_int(val, len, &info->retain) <= 0) {
				spa_log_error(p->log, "onnx: %s expects an int", key);
				return -EINVAL;
			}
		} else if (spa_streq(key, "data")) {
			if (spa_json_parse_stringn(val, len, data, sizeof(data)) <= 0) {
				spa_log_error(p->log, "onnx: %s expects a string", key);
				return -EINVAL;
			}
			if (spa_strstartswith(data, "tensor:")) {
				struct tensor_info *ti;
				spa_scnprintf(info->data_name, sizeof(info->data_name), "%s", data+7);
				ti = find_tensor(d, info->data_name, SPA_DIRECTION_REVERSE(info->direction));
				if (ti == NULL) {
					spa_log_error(p->log, "onnx: unknown tensor %s", info->data_name);
					return -EINVAL;
				}
				info->data_type = DATA_TENSOR;
				info->data_index = ti->index;
			}
			else if (spa_strstartswith(data, "param:rate")) {
				info->data_type = DATA_PARAM_RATE;
			}
			else if (spa_strstartswith(data, "port:")) {
				info->data_type = DATA_PORT;
				spa_scnprintf(info->data_name, sizeof(info->data_name), "%s", data+5);
			}
			else if (spa_strstartswith(data, "control:")) {
				info->data_type = DATA_CONTROL;
				spa_scnprintf(info->data_name, sizeof(info->data_name), "%s", data+8);
			}
			else {
				spa_log_warn(p->log, "onnx: unknown %s value: %s", key, data);
			}
		} else {
			spa_log_warn(p->log, "unexpected onnx tensor-info key '%s'", key);
		}
	}
	return 0;
}

/*
 * {
 *   <name> = {
 *      <tensor-info>
 *   }
 *   ....
 * }
 */
static int parse_tensors(struct descriptor *d, struct spa_json *it, enum spa_direction direction)
{
	struct plugin *p = d->p;
	struct spa_json sub;
	const char *val;
	int len, res;
	char key[256];

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		struct tensor_info *info;

		if ((info = find_tensor(d, key, direction)) == NULL) {
			spa_log_error(p->log, "onnx: unknown tensor name %s", key);
			return -EINVAL;
		}
		if (!spa_json_is_object(val, len)) {
			spa_log_error(p->log, "onnx: tensors %s expects an object", key);
			return -EINVAL;
		}
		spa_json_enter(it, &sub);
		if ((res = parse_tensor_info(d, &sub, info)) < 0)
			return res;
	}
	return 0;
}

#define SET_VAL(data,type,val) \
{  type _v = (type) (val); memcpy(data,	&_v, sizeof(_v)); }	\

static int set_value(void *data, enum ONNXTensorElementDataType type, double val)
{
	switch (type) {
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
		SET_VAL(data, uint8_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
		SET_VAL(data, int8_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
		SET_VAL(data, uint16_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
		SET_VAL(data, int16_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
		SET_VAL(data, int32_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
		SET_VAL(data, int64_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
		SET_VAL(data, bool, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
		SET_VAL(data, double, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
		SET_VAL(data, uint32_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
		SET_VAL(data, uint64_t, val);
		break;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
		SET_VAL(data, float, val);
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}
/*
 * config = {
 *   blocksize = 512
 *   input-tensors = {
 *     <tensors>
 *     ...
 *   }
 *   output-tensors = {
 *     <tensors>
 *     ...
 *   }
 * }
 */

static void *onnx_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor *desc,
                        unsigned long SampleRate, int index, const char *config)
{
	struct descriptor *d = (struct descriptor *)desc;
	struct plugin *p = d->p;
	struct instance *i;
	OrtStatus *status;
	size_t n, j;
	int res;

	errno = EINVAL;

	i = calloc(1, sizeof(*i));
	if (i == NULL)
		return NULL;

	i->desc = d;
	i->rate = SampleRate;

	for (n = 0; n < d->n_tensors; n++) {
		struct tensor_info *ti = &d->tensors[n];
		void *data;

		spa_log_debug(p->log, "%zd %s %zd", n, ti->name, ti->n_dimensions);
		ti->data_size = 1;
		for (j = 0; j < ti->n_dimensions; j++) {
			spa_log_debug(p->log, "%zd %zd/%zd %"PRIi64, n, j, ti->n_dimensions,
					ti->dimensions[j]);
			if (ti->dimensions[j] != -1)
				ti->data_size *= ti->dimensions[j];

		}
		CHECK(ort->CreateTensorAsOrtValue(p->allocator, ti->dimensions, ti->n_dimensions,
				ti->type, &i->tensor[n]));
		CHECK(ort->GetTensorMutableData(i->tensor[n], (void**)&data));

		if (ti->data_type == DATA_PARAM_RATE) {
			if ((res = set_value(data, ti->type, (double)i->rate)) < 0) {
				errno = -res;
				goto error;
			}

		}
	}
	return i;

error_onnx:
	const char* msg = ort->GetErrorMessage(status);
	spa_log_error(p->log, "%s", msg);
	ort->ReleaseStatus(status);
error:
	free(i);
	return NULL;
}

static void onnx_cleanup(void *instance)
{
	struct instance *i = instance;
	free(i);
}

static void onnx_free(const struct spa_fga_descriptor *desc)
{
	struct descriptor *d = (struct descriptor*)desc;
	free((char*)d->desc.name);
	free(d->desc.ports);
	free(d);
}

static void onnx_connect_port(void *instance, unsigned long port, void *data)
{
	struct instance *i = instance;
	i->data[port] = data;
}

static void move_samples(float *dst, uint32_t dst_offs, float *src, uint32_t src_offs, uint32_t n_samples)
{
	memmove(SPA_PTROFF(dst, dst_offs * sizeof(float), void),
		SPA_PTROFF(src, src_offs * sizeof(float), void), n_samples * sizeof(float));
}

static void onnx_run(void *instance, unsigned long SampleCount)
{
	OrtStatus *status;
	struct instance *i = instance;
	struct descriptor *d = i->desc;
	struct plugin *p = d->p;
	const char *input_names[MAX_PORTS];
	const OrtValue *inputs[MAX_PORTS];
	const char *output_names[MAX_PORTS];
	OrtValue *outputs[MAX_PORTS];
	size_t n, n_inputs = 0, n_outputs = 0;
	float *data;
	uint32_t offset = i->offset, blocksize = d->blocksize;

	while (SampleCount > 0) {
		uint32_t chunk = SPA_MIN(SampleCount, blocksize - offset);
		uint32_t next_offset;

		for (n = 0; n < d->n_tensors; n++) {
			struct tensor_info *ti = &d->tensors[n];
			if (ti->direction == SPA_DIRECTION_INPUT) {
				input_names[n_inputs] = ti->name;
				inputs[n_inputs++] = i->tensor[ti->index];

				if (ti->data_type == DATA_PORT) {
					CHECK(ort->GetTensorMutableData(i->tensor[ti->index], (void**)&data));

					if (ti->retain > 0 && offset == 0)
						move_samples(data, 0, data, ti->data_size - ti->retain, ti->retain);

					move_samples(data, ti->retain + offset,
							i->data[ti->data_index], offset, chunk);
				}
				else if (ti->data_type == DATA_TENSOR) {
					if (offset == 0) {
						void *src, *dst;
						CHECK(ort->GetTensorMutableData(i->tensor[ti->data_index], &src));
						CHECK(ort->GetTensorMutableData(i->tensor[ti->index], &dst));
						move_samples(dst, 0, src, 0, ti->data_size);
					}
				}
			} else {
				output_names[n_outputs] = ti->name;
				outputs[n_outputs++] = i->tensor[ti->index];
			}
		}
		if (offset + chunk >= blocksize) {
			CHECK(ort->Run(d->session, i->run_options,
					input_names, (const OrtValue *const*)inputs, n_inputs,
					output_names, n_outputs, (OrtValue **)outputs));
			next_offset = 0;
		} else {
			next_offset = offset + chunk;
		}

		for (n = 0; n < d->n_tensors; n++) {
			struct tensor_info *ti = &d->tensors[n];
			if (ti->direction != SPA_DIRECTION_OUTPUT)
				continue;

			if (ti->data_type == DATA_CONTROL) {
				if (next_offset == 0) {
					float *src, *dst;
					CHECK(ort->GetTensorMutableData(i->tensor[ti->index], (void**)&src));
					dst = i->data[ti->data_index];
					if (src && dst)
						dst[0] = src[0];
				}
			}
			else if (ti->data_type == DATA_PORT) {
				CHECK(ort->GetTensorMutableData(i->tensor[ti->index], (void**)&data));
				move_samples(i->data[ti->data_index], offset, data, offset, chunk);
			}
		}
		SampleCount -= chunk;
		offset = next_offset;
	}
	i->offset = offset;
	return;

error_onnx:
	const char* msg = ort->GetErrorMessage(status);
	spa_log_error(p->log, "%s", msg);
	ort->ReleaseStatus(status);
}

static const struct spa_fga_descriptor *onnx_plugin_make_desc(void *plugin, const char *name)
{
	OrtStatus *status;
	struct plugin *p = (struct plugin *)plugin;
	struct descriptor *desc;
	size_t i, j, n_inputs, n_outputs;
	OrtTypeInfo *tinfo;
	const OrtTensorTypeAndShapeInfo *tt;
	char path[PATH_MAX];
	struct spa_json it[2];
	const char *val;
	int len;
	char key[256];

	if (spa_json_begin_object(&it[0], name, strlen(name)) <= 0) {
		spa_log_error(p->log, "onnx: expected object in label");
		return NULL;
	}
	if (spa_json_str_object_find(name, strlen(name), "filename", path, sizeof(path)) <= 0) {
		spa_log_error(p->log, "onnx: could not find filename in label");
		return NULL;
	}

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL)
		return NULL;

	desc->p = p;

	desc->desc.instantiate = onnx_instantiate;
	desc->desc.cleanup = onnx_cleanup;
	desc->desc.free = onnx_free;
	desc->desc.connect_port = onnx_connect_port;
	desc->desc.run = onnx_run;

	desc->desc.name = strdup(name);
	desc->desc.flags = 0;

	spa_log_info(p->log, "onnx: loading model %s", path);
	CHECK(ort->CreateSession(p->env, path, p->session_options, &desc->session));

	CHECK(ort->SessionGetInputCount(desc->session, &n_inputs));
	CHECK(ort->SessionGetOutputCount(desc->session, &n_outputs));

	spa_log_info(p->log, "found %zd input and %zd output tensors", n_inputs, n_outputs);

	/* first go over all tensors and collect info */
	for (i = 0; i < n_inputs; i++) {
		struct tensor_info *ti = &desc->tensors[i];

		ti->index = i;
		ti->direction = SPA_DIRECTION_INPUT;
		CHECK(ort->SessionGetInputName(desc->session, i, p->allocator, (char**)&ti->name));

		CHECK(ort->SessionGetInputTypeInfo(desc->session, i, &tinfo));
		CHECK(ort->CastTypeInfoToTensorInfo(tinfo, &tt));

		CHECK(ort->GetTensorElementType(tt, &ti->type));
		CHECK(ort->GetDimensionsCount(tt, &ti->n_dimensions));
		if (ti->n_dimensions > SPA_N_ELEMENTS(ti->dimensions)) {
			spa_log_warn(p->log, "too many dimensions");
			errno = ENOTSUP;
			goto error;
		}
		CHECK(ort->GetDimensions(tt, ti->dimensions, ti->n_dimensions));

		spa_log_debug(p->log, "%zd %s %zd", i, ti->name, ti->n_dimensions);
		for (j = 0; j < ti->n_dimensions; j++) {
			spa_log_debug(p->log, "%zd %zd/%zd %"PRIi64, i, j, ti->n_dimensions,
					ti->dimensions[j]);
		}
	}
	for (i = 0; i < n_outputs; i++) {
		struct tensor_info *ti = &desc->tensors[i + n_inputs];

		ti->index = i + n_inputs;
		ti->direction = SPA_DIRECTION_OUTPUT;
		CHECK(ort->SessionGetOutputName(desc->session, i, p->allocator, (char**)&ti->name));

		CHECK(ort->SessionGetOutputTypeInfo(desc->session, i, &tinfo));
		CHECK(ort->CastTypeInfoToTensorInfo(tinfo, &tt));

		CHECK(ort->GetTensorElementType(tt, &ti->type));
		CHECK(ort->GetDimensionsCount(tt, &ti->n_dimensions));
		if (ti->n_dimensions > SPA_N_ELEMENTS(ti->dimensions)) {
			spa_log_error(p->log, "too many dimensions");
			errno = ENOTSUP;
			goto error;
		}
		CHECK(ort->GetDimensions(tt, ti->dimensions, ti->n_dimensions));

		spa_log_debug(p->log, "%zd %s %zd", i, ti->name, ti->n_dimensions);
		for (j = 0; j < ti->n_dimensions; j++) {
			spa_log_debug(p->log, "%zd %zd/%zd %"PRIi64, i, j, ti->n_dimensions,
					ti->dimensions[j]);
		}
	}
	desc->n_tensors = n_inputs + n_outputs;

	/* enhance the tensor info */
	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "blocksize")) {
			if (spa_json_parse_int(val, len, &desc->blocksize) <= 0) {
				spa_log_error(p->log, "onnx:blocksize requires a number");
				errno = EINVAL;
				goto error;
			}
		}
		else if (spa_streq(key, "input-tensors")) {
			if (!spa_json_is_object(val, len)) {
				spa_log_error(p->log, "onnx: %s expects an object", key);
				errno = EINVAL;
				goto error;
			}
			spa_json_enter(&it[0], &it[1]);
			parse_tensors(desc, &it[1], SPA_DIRECTION_INPUT);
		}
		else if (spa_streq(key, "output-tensors")) {
			if (!spa_json_is_object(val, len)) {
				spa_log_error(p->log, "onnx: %s expects an object", key);
				errno = EINVAL;
				goto error;
			}
			spa_json_enter(&it[0], &it[1]);
			parse_tensors(desc, &it[1], SPA_DIRECTION_OUTPUT);
		}
	}

	desc->desc.ports = calloc(desc->n_tensors, sizeof(struct spa_fga_port));
	desc->desc.n_ports = 0;

	/* make ports */
	for (i = 0; i < desc->n_tensors; i++) {
		struct tensor_info *ti = &desc->tensors[i];
		struct spa_fga_port *fp = &desc->desc.ports[desc->desc.n_ports];

		fp->flags = 0;
		fp->index = desc->desc.n_ports;
		if (ti->type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			continue;

		if (ti->data_type == DATA_PORT)
			fp->flags |= SPA_FGA_PORT_AUDIO;
		else if (ti->data_type == DATA_CONTROL)
			fp->flags |= SPA_FGA_PORT_CONTROL;
		else
			continue;

		if (ti->direction == SPA_DIRECTION_INPUT)
			fp->flags |= SPA_FGA_PORT_INPUT;
		else
			fp->flags |= SPA_FGA_PORT_OUTPUT;

		fp->name = ti->data_name;
		ti->data_index = desc->desc.n_ports;

		desc->desc.n_ports++;
		if (desc->desc.n_ports > MAX_PORTS) {
			spa_log_error(p->log, "too many ports");
			errno = -ENOSPC;
			goto error;
		}
	}
	return &desc->desc;

error_onnx:
	const char* msg = ort->GetErrorMessage(status);
	spa_log_error(p->log, "%s", msg);
	ort->ReleaseStatus(status);

error:
	if (desc->session)
		ort->ReleaseSession(desc->session);
	onnx_free(&desc->desc);
	return NULL;
}

static int load_model(struct plugin *impl, const char *path)
{
	OrtStatus *status;

	ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
	if (ort == NULL) {
		spa_log_error(impl->log, "Failed to init ONNX Runtime engine");
		return -EINVAL;
	}
	CHECK(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "onnx-filter-graph", &impl->env));
	CHECK(ort->GetAllocatorWithDefaultOptions(&impl->allocator));

	CHECK(ort->CreateSessionOptions(&impl->session_options));
	CHECK(ort->SetIntraOpNumThreads(impl->session_options, 1));
	CHECK(ort->SetInterOpNumThreads(impl->session_options, 1));
	CHECK(ort->SetSessionGraphOptimizationLevel(impl->session_options, ORT_ENABLE_ALL));

	return 0;

error_onnx:
	const char* msg = ort->GetErrorMessage(status);
	spa_log_error(impl->log, "%s", msg);
	ort->ReleaseStatus(status);

	if (impl->env)
		ort->ReleaseEnv(impl->env);
	impl->env = NULL;
	if (impl->session_options)
		ort->ReleaseSessionOptions(impl->session_options);
	impl->session_options = NULL;

	return -EINVAL;
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = onnx_plugin_make_desc,
};

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
	const char *path = NULL;
	int res;

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

	if ((res = load_model(impl, path)) < 0)
		return res;

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

static struct spa_handle_factory spa_fga_plugin_onnx_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.onnx",
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
		*factory = &spa_fga_plugin_onnx_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
