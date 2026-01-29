#include "config.h"

#include <limits.h>

#include <spa/utils/json.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>

#include "audio-plugin.h"
#include "convolver.h"
#include "audio-dsp.h"

#include <mysofa.h>

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_loop *main_loop;
	uint32_t quantum_limit;
};

struct spatializer_impl {
	struct plugin *plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;

	unsigned long rate;
	float *port[7];
	int n_samples, blocksize, tailsize;
	float *tmp[2];

	struct MYSOFA_EASY *sofa;
	unsigned int interpolate:1;
	struct convolver *l_conv[3];
	struct convolver *r_conv[3];
};

static void * spatializer_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct spatializer_impl *impl;
	struct spa_json it[1];
	const char *val;
	char key[256];
	char filename[PATH_MAX] = "";
	int len;

	errno = EINVAL;
	if (config == NULL) {
		spa_log_error(pl->log, "spatializer: no config was given");
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(pl->log, "spatializer: expected object in config");
		return NULL;
	}

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	impl->plugin = pl;
	impl->dsp = pl->dsp;
	impl->log = pl->log;

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "blocksize")) {
			if (spa_json_parse_int(val, len, &impl->blocksize) <= 0) {
				spa_log_error(impl->log, "spatializer:blocksize requires a number");
				errno = EINVAL;
				goto error;
			}
		}
		else if (spa_streq(key, "tailsize")) {
			if (spa_json_parse_int(val, len, &impl->tailsize) <= 0) {
				spa_log_error(impl->log, "spatializer:tailsize requires a number");
				errno = EINVAL;
				goto error;
			}
		}
		else if (spa_streq(key, "filename")) {
			if (spa_json_parse_stringn(val, len, filename, sizeof(filename)) <= 0) {
				spa_log_error(impl->log, "spatializer:filename requires a string");
				errno = EINVAL;
				goto error;
			}
		}
	}
	if (!filename[0]) {
		spa_log_error(impl->log, "spatializer:filename was not given");
		errno = EINVAL;
		goto error;
	}

	int ret = MYSOFA_OK;

	impl->sofa = mysofa_open_cached(filename, SampleRate, &impl->n_samples, &ret);

	if (ret != MYSOFA_OK) {
		const char *reason;
		switch (ret) {
		case MYSOFA_INVALID_FORMAT:
			reason = "Invalid format";
			errno = EINVAL;
			break;
		case MYSOFA_UNSUPPORTED_FORMAT:
			reason = "Unsupported format";
			errno = ENOTSUP;
			break;
		case MYSOFA_NO_MEMORY:
			reason = "No memory";
			errno = ENOMEM;
			break;
		case MYSOFA_READ_ERROR:
			reason = "Read error";
			errno = ENOENT;
			break;
		case MYSOFA_INVALID_ATTRIBUTES:
			reason = "Invalid attributes";
			errno = EINVAL;
			break;
		case MYSOFA_INVALID_DIMENSIONS:
			reason = "Invalid dimensions";
			errno = EINVAL;
			break;
		case MYSOFA_INVALID_DIMENSION_LIST:
			reason = "Invalid dimension list";
			errno = EINVAL;
			break;
		case MYSOFA_INVALID_COORDINATE_TYPE:
			reason = "Invalid coordinate type";
			errno = EINVAL;
			break;
		case MYSOFA_ONLY_EMITTER_WITH_ECI_SUPPORTED:
			reason = "Only emitter with ECI supported";
			errno = ENOTSUP;
			break;
		case MYSOFA_ONLY_DELAYS_WITH_IR_OR_MR_SUPPORTED:
			reason = "Only delays with IR or MR supported";
			errno = ENOTSUP;
			break;
		case MYSOFA_ONLY_THE_SAME_SAMPLING_RATE_SUPPORTED:
			reason = "Only the same sampling rate supported";
			errno = ENOTSUP;
			break;
		case MYSOFA_RECEIVERS_WITH_RCI_SUPPORTED:
			reason = "Receivers with RCI supported";
			errno = ENOTSUP;
			break;
		case MYSOFA_RECEIVERS_WITH_CARTESIAN_SUPPORTED:
			reason = "Receivers with cartesian supported";
			errno = ENOTSUP;
			break;
		case MYSOFA_INVALID_RECEIVER_POSITIONS:
			reason = "Invalid receiver positions";
			errno = EINVAL;
			break;
		case MYSOFA_ONLY_SOURCES_WITH_MC_SUPPORTED:
			reason = "Only sources with MC supported";
			errno = ENOTSUP;
			break;
		case MYSOFA_INTERNAL_ERROR:
			errno = EIO;
			reason = "Internal error";
			break;
		default:
			errno = ret;
			reason = strerror(errno);
			break;
		}
		spa_log_error(impl->log, "Unable to load HRTF from %s: %s (%d)", filename, reason, ret);
		goto error;
	}

	if (impl->blocksize <= 0)
		impl->blocksize = SPA_CLAMP(impl->n_samples, 64, 256);
	if (impl->tailsize <= 0)
		impl->tailsize = SPA_CLAMP(4096, impl->blocksize, 32768);

	spa_log_info(impl->log, "using n_samples:%u %d:%d blocksize sofa:%s", impl->n_samples,
		impl->blocksize, impl->tailsize, filename);

	impl->tmp[0] = calloc(impl->plugin->quantum_limit, sizeof(float));
	impl->tmp[1] = calloc(impl->plugin->quantum_limit, sizeof(float));
	impl->rate = SampleRate;
	return impl;
error:
	if (impl->sofa)
		mysofa_close_cached(impl->sofa);
	free(impl);
	return NULL;
}

static int
do_switch(struct spa_loop *loop, bool async, uint32_t seq, const void *data,
		size_t size, void *user_data)
{
	struct spatializer_impl *impl = user_data;

	if (impl->l_conv[0] == NULL) {
		SPA_SWAP(impl->l_conv[0], impl->l_conv[2]);
		SPA_SWAP(impl->r_conv[0], impl->r_conv[2]);
	} else {
		SPA_SWAP(impl->l_conv[1], impl->l_conv[2]);
		SPA_SWAP(impl->r_conv[1], impl->r_conv[2]);
	}
	impl->interpolate = impl->l_conv[0] && impl->l_conv[1];

	return 0;
}

static void spatializer_reload(void * Instance)
{
	struct spatializer_impl *impl = Instance;
	float *left_ir = calloc(impl->n_samples, sizeof(float));
	float *right_ir = calloc(impl->n_samples, sizeof(float));
	float left_delay;
	float right_delay;
	float coords[3];

	for (uint8_t i = 0; i < 3; i++)
		coords[i] = impl->port[3 + i][0];

	spa_log_info(impl->log, "making spatializer with %f %f %f", coords[0], coords[1], coords[2]);

	mysofa_s2c(coords);
	mysofa_getfilter_float(
		impl->sofa,
		coords[0],
		coords[1],
		coords[2],
		left_ir,
		right_ir,
		&left_delay,
		&right_delay
	);

	// TODO: make use of delay
	if ((left_delay != 0.0f || right_delay != 0.0f) && (!isnan(left_delay) || !isnan(right_delay)))
		spa_log_warn(impl->log, "delay dropped l: %f, r: %f", left_delay, right_delay);

	if (impl->l_conv[2])
		convolver_free(impl->l_conv[2]);
	if (impl->r_conv[2])
		convolver_free(impl->r_conv[2]);

	impl->l_conv[2] = convolver_new(impl->dsp, impl->blocksize, impl->tailsize,
			left_ir, impl->n_samples);
	impl->r_conv[2] = convolver_new(impl->dsp, impl->blocksize, impl->tailsize,
			right_ir, impl->n_samples);

	free(left_ir);
	free(right_ir);

	if (impl->l_conv[2] == NULL || impl->r_conv[2] == NULL) {
		spa_log_error(impl->log, "reloading left or right convolver failed");
		return;
	}
	spa_loop_locked(impl->plugin->data_loop, do_switch, 1, NULL, 0, impl);
}

struct free_data {
	void *item[2];
};

static int
do_free(struct spa_loop *loop, bool async, uint32_t seq, const void *data,
		size_t size, void *user_data)
{
	const struct free_data *fd = data;
	if (fd->item[0])
		convolver_free(fd->item[0]);
	if (fd->item[1])
		convolver_free(fd->item[1]);
	return 0;
}

static void spatializer_run(void * Instance, unsigned long SampleCount)
{
	struct spatializer_impl *impl = Instance;

	if (impl->interpolate) {
		uint32_t len = SPA_MIN(SampleCount, impl->plugin->quantum_limit);
		struct free_data free_data;
		float *l = impl->tmp[0], *r = impl->tmp[1];

		convolver_run(impl->l_conv[0], impl->port[2], impl->port[0], len);
		convolver_run(impl->l_conv[1], impl->port[2], l, len);
		convolver_run(impl->r_conv[0], impl->port[2], impl->port[1], len);
		convolver_run(impl->r_conv[1], impl->port[2], r, len);

		for (uint32_t i = 0; i < SampleCount; i++) {
			float t = (float)i / SampleCount;
			impl->port[0][i] = impl->port[0][i] * (1.0f - t) + l[i] * t;
			impl->port[1][i] = impl->port[1][i] * (1.0f - t) + r[i] * t;
		}
		free_data.item[0] = impl->l_conv[0];
		free_data.item[1] = impl->r_conv[0];
		impl->l_conv[0] = impl->l_conv[1];
		impl->r_conv[0] = impl->r_conv[1];
		impl->l_conv[1] = impl->r_conv[1] = NULL;
		impl->interpolate = false;

		spa_loop_invoke(impl->plugin->main_loop, do_free, 1, &free_data, sizeof(free_data), false, impl);
	} else if (impl->l_conv[0] && impl->r_conv[0]) {
		convolver_run(impl->l_conv[0], impl->port[2], impl->port[0], SampleCount);
		convolver_run(impl->r_conv[0], impl->port[2], impl->port[1], SampleCount);
	}
	impl->port[6][0] = impl->n_samples;
}

static void spatializer_connect_port(void * Instance, unsigned long Port,
                        void * DataLocation)
{
	struct spatializer_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void spatializer_cleanup(void * Instance)
{
	struct spatializer_impl *impl = Instance;

	for (uint8_t i = 0; i < 3; i++) {
		if (impl->l_conv[i])
			convolver_free(impl->l_conv[i]);
		if (impl->r_conv[i])
			convolver_free(impl->r_conv[i]);
	}
	if (impl->sofa)
		mysofa_close_cached(impl->sofa);
	free(impl->tmp[0]);
	free(impl->tmp[1]);

	free(impl);
}

static void spatializer_control_changed(void * Instance)
{
	spatializer_reload(Instance);
}

static void spatializer_activate(void * Instance)
{
	struct spatializer_impl *impl = Instance;
	impl->port[6][0] = impl->n_samples;
}

static void spatializer_deactivate(void * Instance)
{
	struct spatializer_impl *impl = Instance;
	if (impl->l_conv[0])
		convolver_reset(impl->l_conv[0]);
	if (impl->r_conv[0])
		convolver_reset(impl->r_conv[0]);
	impl->interpolate = false;
}

static struct spa_fga_port spatializer_ports[] = {
	{ .index = 0,
	  .name = "Out L",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Out R",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = 3,
	  .name = "Azimuth",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = 0.0f, .max = 360.0f
	},
	{ .index = 4,
	  .name = "Elevation",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 0.0f, .min = -90.0f, .max = 90.0f
	},
	{ .index = 5,
	  .name = "Radius",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 100.0f
	},
	{ .index = 6,
	  .name = "latency",
	  .hint = SPA_FGA_HINT_LATENCY,
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
};

static const struct spa_fga_descriptor spatializer_desc = {
	.name = "spatializer",

	.n_ports = SPA_N_ELEMENTS(spatializer_ports),
	.ports = spatializer_ports,

	.instantiate = spatializer_instantiate,
	.connect_port = spatializer_connect_port,
	.control_changed = spatializer_control_changed,
	.activate = spatializer_activate,
	.deactivate = spatializer_deactivate,
	.run = spatializer_run,
	.cleanup = spatializer_cleanup,
};

static const struct spa_fga_descriptor * sofa_descriptor(unsigned long Index)
{
	switch(Index) {
	case 0:
		return &spatializer_desc;
	}
	return NULL;
}


static const struct spa_fga_descriptor *sofa_plugin_make_desc(void *plugin, const char *name)
{
	unsigned long i;
	for (i = 0; ;i++) {
		const struct spa_fga_descriptor *d = sofa_descriptor(i);
		if (d == NULL)
			break;
		if (spa_streq(d->name, name))
			return d;
	}
	return NULL;
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = sofa_plugin_make_desc,
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

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct plugin *) handle;

	impl->plugin.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,
			SPA_VERSION_FGA_PLUGIN,
			&impl_plugin, impl);

	impl->quantum_limit = 8192u;

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	impl->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	impl->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	impl->dsp = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioDSP);

	for (uint32_t i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit"))
			spa_atou32(s, &impl->quantum_limit, 0);
		if (spa_streq(k, "filter.graph.audio.dsp"))
			sscanf(s, "pointer:%p", &impl->dsp);
	}

	if (impl->data_loop == NULL || impl->main_loop == NULL) {
		spa_log_error(impl->log, "%p: could not find a data/main loop", impl);
		return -EINVAL;
	}
	if (impl->dsp == NULL) {
		spa_log_error(impl->log, "%p: could not find DSP functions", impl);
		return -EINVAL;
	}
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,},
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

static struct spa_handle_factory spa_fga_sofa_plugin_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.sofa",
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
		*factory = &spa_fga_sofa_plugin_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
