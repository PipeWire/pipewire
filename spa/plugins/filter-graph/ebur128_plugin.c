#include "config.h"

#include <limits.h>

#include <spa/utils/json.h>
#include <spa/support/log.h>

#include "audio-plugin.h"
#include "audio-dsp.h"

#include <ebur128.h>

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;
	uint32_t quantum_limit;
};

enum {
	PORT_IN_FL,
	PORT_IN_FR,
	PORT_IN_FC,
	PORT_IN_UNUSED,
	PORT_IN_SL,
	PORT_IN_SR,
	PORT_IN_DUAL_MONO,

	PORT_OUT_FL,
	PORT_OUT_FR,
	PORT_OUT_FC,
	PORT_OUT_UNUSED,
	PORT_OUT_SL,
	PORT_OUT_SR,
	PORT_OUT_DUAL_MONO,

	PORT_OUT_MOMENTARY,
	PORT_OUT_SHORTTERM,
	PORT_OUT_GLOBAL,
	PORT_OUT_WINDOW,
	PORT_OUT_RANGE,
	PORT_OUT_PEAK,
	PORT_OUT_TRUE_PEAK,

	PORT_MAX,

	PORT_IN_START = PORT_IN_FL,
	PORT_OUT_START = PORT_OUT_FL,
	PORT_NOTIFY_START = PORT_OUT_MOMENTARY,
};


struct ebur128_impl {
	struct plugin *plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;

	unsigned long rate;
	float *port[PORT_MAX];

	unsigned int max_history;
	unsigned int max_window;
	bool use_histogram;

	ebur128_state *st[7];
};

static void * ebur128_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct ebur128_impl *impl;
	struct spa_json it[1];
	const char *val;
	char key[256];
	int len;
	float f;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	impl->plugin = pl;
	impl->dsp = pl->dsp;
	impl->log = pl->log;
	impl->max_history = 10000;
	impl->max_window = 0;
	impl->rate = SampleRate;

	if (config == NULL)
		return impl;

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(pl->log, "ebur128: expected object in config");
		errno = EINVAL;
		goto error;
	}
	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "max-history")) {
			if (spa_json_parse_float(val, len, &f) <= 0) {
				spa_log_error(impl->log, "ebur128:max-history requires a number");
				errno = EINVAL;
				goto error;
			}
			impl->max_history = (unsigned int) (f * 1000.0f);
		}
		else if (spa_streq(key, "max-window")) {
			if (spa_json_parse_float(val, len, &f) <= 0) {
				spa_log_error(impl->log, "ebur128:max-window requires a number");
				errno = EINVAL;
				goto error;
			}
			impl->max_window = (unsigned int) (f * 1000.0f);
		}
		else if (spa_streq(key, "use-histogram")) {
			if (spa_json_parse_bool(val, len, &impl->use_histogram) <= 0) {
				spa_log_error(impl->log, "ebur128:use-histogram requires a boolean");
				errno = EINVAL;
				goto error;
			}
		} else {
			spa_log_warn(impl->log, "ebur128: unknown key %s", key);
		}
	}
	return impl;
error:
	free(impl);
	return NULL;
}

static void ebur128_run(void * Instance, unsigned long SampleCount)
{
	struct ebur128_impl *impl = Instance;
	int i, c;
	double value;
	ebur128_state *st[7];

	for (i = 0; i < 7; i++) {
		float *in = impl->port[PORT_IN_START + i];
		float *out = impl->port[PORT_OUT_START + i];

		st[i] = NULL;
		if (in == NULL)
			continue;

		st[i] = impl->st[i];
		if (st[i] != NULL)
			ebur128_add_frames_float(st[i], in, SampleCount);

		if (out != NULL)
			memcpy(out, in, SampleCount * sizeof(float));
	}
	if (impl->port[PORT_OUT_MOMENTARY] != NULL) {
		double sum = 0.0;
		for (i = 0, c = 0; i < 7; i++) {
			if (st[i] != NULL) {
				ebur128_loudness_momentary(st[i], &value);
				sum += value;
				c++;
			}
		}
		impl->port[PORT_OUT_MOMENTARY][0] = (float) (sum / c);
	}
	if (impl->port[PORT_OUT_SHORTTERM] != NULL) {
		double sum = 0.0;
		for (i = 0, c = 0; i < 7; i++) {
			if (st[i] != NULL) {
				ebur128_loudness_shortterm(st[i], &value);
				sum += value;
				c++;
			}
		}
		impl->port[PORT_OUT_SHORTTERM][0] = (float) (sum / c);
	}
	if (impl->port[PORT_OUT_GLOBAL] != NULL) {
		ebur128_loudness_global_multiple(st, 7, &value);
		impl->port[PORT_OUT_GLOBAL][0] = (float)value;
	}
	if (impl->port[PORT_OUT_WINDOW] != NULL) {
		double sum = 0.0;
		for (i = 0, c = 0; i < 7; i++) {
			if (st[i] != NULL) {
				ebur128_loudness_window(st[i], impl->max_window, &value);
				sum += value;
				c++;
			}
		}
		impl->port[PORT_OUT_WINDOW][0] = (float) (sum / c);
	}
	if (impl->port[PORT_OUT_RANGE] != NULL) {
		ebur128_loudness_range_multiple(st, 7, &value);
		impl->port[PORT_OUT_RANGE][0] = (float)value;
	}
	if (impl->port[PORT_OUT_PEAK] != NULL) {
		double max = 0.0;
		for (i = 0; i < 7; i++) {
			if (st[i] != NULL) {
				ebur128_sample_peak(st[i], i, &value);
				max = SPA_MAX(max, value);
			}
		}
		impl->port[PORT_OUT_PEAK][0] = (float) max;
	}
	if (impl->port[PORT_OUT_TRUE_PEAK] != NULL) {
		double max = 0.0;
		for (i = 0; i < 7; i++) {
			if (st[i] != NULL) {
				ebur128_true_peak(st[i], i, &value);
				max = SPA_MAX(max, value);
			}
		}
		impl->port[PORT_OUT_TRUE_PEAK][0] = (float) max;
	}
}

static void ebur128_connect_port(void * Instance, unsigned long Port,
                        float * DataLocation)
{
	struct ebur128_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void ebur128_cleanup(void * Instance)
{
	struct ebur128_impl *impl = Instance;
	free(impl);
}

static void ebur128_activate(void * Instance)
{
	struct ebur128_impl *impl = Instance;
	unsigned long max_window;
	int major, minor, patch;
	int mode = 0, i;
	int modes[] = {
		EBUR128_MODE_M,
  		EBUR128_MODE_S,
  		EBUR128_MODE_I,
		0,
  		EBUR128_MODE_LRA,
  		EBUR128_MODE_SAMPLE_PEAK,
  		EBUR128_MODE_TRUE_PEAK,
	};
	enum channel channels[] = {
		EBUR128_LEFT,
		EBUR128_RIGHT,
		EBUR128_CENTER,
		EBUR128_UNUSED,
		EBUR128_LEFT_SURROUND,
		EBUR128_RIGHT_SURROUND,
		EBUR128_DUAL_MONO,
	};

	if (impl->use_histogram)
		mode |= EBUR128_MODE_HISTOGRAM;

	/* check modes */
	for (i = 0; i < 7; i++) {
		if (impl->port[PORT_NOTIFY_START + i] != NULL)
			mode |= modes[i];
	}

	ebur128_get_version(&major, &minor, &patch);
	max_window = impl->max_window;
	if (major == 1 && minor == 2 && (patch == 5 || patch == 6))
		max_window = (max_window + 999) / 1000;

	for (i = 0; i < 7; i++) {
		impl->st[i] = ebur128_init(1, impl->rate, mode);
		if (impl->st[i]) {
			ebur128_set_channel(impl->st[i], i, channels[i]);
			ebur128_set_max_history(impl->st[i], impl->max_history);
			ebur128_set_max_window(impl->st[i], max_window);
		}
	}
}

static void ebur128_deactivate(void * Instance)
{
	struct ebur128_impl *impl = Instance;
	int i;

	for (i = 0; i < 7; i++) {
		if (impl->st[i] != NULL)
			ebur128_destroy(&impl->st[i]);
	}
}

static struct spa_fga_port ebur128_ports[] = {
	{ .index = PORT_IN_FL,
	  .name = "In FL",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_IN_FR,
	  .name = "In FR",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_IN_FC,
	  .name = "In FC",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_IN_UNUSED,
	  .name = "In UNUSED",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_IN_SL,
	  .name = "In SL",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_IN_SR,
	  .name = "In SR",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_IN_DUAL_MONO,
	  .name = "In DUAL MONO",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = PORT_OUT_FL,
	  .name = "Out FL",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_OUT_FR,
	  .name = "Out FR",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_OUT_FC,
	  .name = "Out FC",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_OUT_UNUSED,
	  .name = "Out UNUSED",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_OUT_SL,
	  .name = "Out SL",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_OUT_SR,
	  .name = "Out SR",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = PORT_OUT_DUAL_MONO,
	  .name = "Out DUAL MONO",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},

	{ .index = PORT_OUT_MOMENTARY,
	  .name = "Momentary LUFS",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = PORT_OUT_SHORTTERM,
	  .name = "Shortterm LUFS",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = PORT_OUT_GLOBAL,
	  .name = "Global LUFS",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = PORT_OUT_WINDOW,
	  .name = "Window LUFS",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = PORT_OUT_RANGE,
	  .name = "Range LU",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = PORT_OUT_PEAK,
	  .name = "Peak",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = PORT_OUT_TRUE_PEAK,
	  .name = "True Peak",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
};

static const struct spa_fga_descriptor ebur128_desc = {
	.name = "ebur128",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.ports = ebur128_ports,
	.n_ports = SPA_N_ELEMENTS(ebur128_ports),

	.instantiate = ebur128_instantiate,
	.connect_port = ebur128_connect_port,
	.activate = ebur128_activate,
	.deactivate = ebur128_deactivate,
	.run = ebur128_run,
	.cleanup = ebur128_cleanup,
};

static struct spa_fga_port lufs2gain_ports[] = {
	{ .index = 0,
	  .name = "LUFS",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 1,
	  .name = "Gain",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL,
	},
	{ .index = 2,
	  .name = "Target LUFS",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL,
	  .def = -23.0f, .min = -70.0f, .max = 0.0f
	},
};

struct lufs2gain_impl {
	struct plugin *plugin;

	struct spa_fga_dsp *dsp;
	struct spa_log *log;

	unsigned long rate;
	float *port[3];
};

static void * lufs2gain_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct lufs2gain_impl *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	impl->plugin = pl;
	impl->dsp = pl->dsp;
	impl->log = pl->log;
	impl->rate = SampleRate;

	return impl;
}

static void lufs2gain_connect_port(void * Instance, unsigned long Port,
                        float * DataLocation)
{
	struct lufs2gain_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void lufs2gain_run(void * Instance, unsigned long SampleCount)
{
	struct lufs2gain_impl *impl = Instance;
	float *in = impl->port[0];
	float *out = impl->port[1];
	float *target = impl->port[2];
	float gain;

	if (in == NULL || out == NULL || target == NULL)
		return;

	if (isfinite(in[0])) {
		float gaindB = target[0] - in[0];
		gain = powf(10.0f, gaindB / 20.0f);
	} else {
		gain = 1.0f;
	}
	out[0] = gain;
}

static void lufs2gain_cleanup(void * Instance)
{
	struct lufs2gain_impl *impl = Instance;
	free(impl);
}

static const struct spa_fga_descriptor lufs2gain_desc = {
	.name = "lufs2gain",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.ports = lufs2gain_ports,
	.n_ports = SPA_N_ELEMENTS(lufs2gain_ports),

	.instantiate = lufs2gain_instantiate,
	.connect_port = lufs2gain_connect_port,
	.run = lufs2gain_run,
	.cleanup = lufs2gain_cleanup,
};

static const struct spa_fga_descriptor * ebur128_descriptor(unsigned long Index)
{
	switch(Index) {
	case 0:
		return &ebur128_desc;
	case 1:
		return &lufs2gain_desc;
	}
	return NULL;
}


static const struct spa_fga_descriptor *ebur128_plugin_make_desc(void *plugin, const char *name)
{
	unsigned long i;
	for (i = 0; ;i++) {
		const struct spa_fga_descriptor *d = ebur128_descriptor(i);
		if (d == NULL)
			break;
		if (spa_streq(d->name, name))
			return d;
	}
	return NULL;
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = ebur128_plugin_make_desc,
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
	impl->dsp = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioDSP);

	for (uint32_t i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit"))
			spa_atou32(s, &impl->quantum_limit, 0);
		if (spa_streq(k, "filter.graph.audio.dsp"))
			sscanf(s, "pointer:%p", &impl->dsp);
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

static struct spa_handle_factory spa_fga_ebur128_plugin_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.ebur128",
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
		*factory = &spa_fga_ebur128_plugin_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
