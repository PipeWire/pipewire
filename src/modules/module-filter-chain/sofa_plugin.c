#include "config.h"

#include <limits.h>

#include <spa/utils/json.h>
#include <spa/support/loop.h>

#include <pipewire/log.h>

#include "plugin.h"
#include "convolver.h"
#include "dsp-ops.h"
#include "pffft.h"

#include <mysofa.h>

#define MAX_SAMPLES	8192u

static struct dsp_ops *dsp_ops;
static struct spa_loop *data_loop;
static struct spa_loop *main_loop;

struct spatializer_impl {
	unsigned long rate;
	float *port[6];
	int n_samples, blocksize, tailsize;
	float *tmp[2];

	struct MYSOFA_EASY *sofa;
	unsigned int interpolate:1;
	struct convolver *l_conv[3];
	struct convolver *r_conv[3];
};

static void * spatializer_instantiate(const struct fc_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct spatializer_impl *impl;
	struct spa_json it[2];
	const char *val;
	char key[256];
	char filename[PATH_MAX] = "";

	errno = EINVAL;
	if (config == NULL)
		return NULL;

	spa_json_init(&it[0], config, strlen(config));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return NULL;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_streq(key, "blocksize")) {
			if (spa_json_get_int(&it[1], &impl->blocksize) <= 0) {
				pw_log_error("spatializer:blocksize requires a number");
				errno = EINVAL;
				goto error;
			}
		}
		else if (spa_streq(key, "tailsize")) {
			if (spa_json_get_int(&it[1], &impl->tailsize) <= 0) {
				pw_log_error("spatializer:tailsize requires a number");
				errno = EINVAL;
				goto error;
			}
		}
		else if (spa_streq(key, "filename")) {
			if (spa_json_get_string(&it[1], filename, sizeof(filename)) <= 0) {
				pw_log_error("spatializer:filename requires a string");
				errno = EINVAL;
				goto error;
			}
		}
		else if (spa_json_next(&it[1], &val) < 0)
			break;
	}
	if (!filename[0]) {
		pw_log_error("spatializer:filename was not given");
		errno = EINVAL;
		goto error;
	}

	int ret = MYSOFA_OK;

	impl->sofa = mysofa_open_cached(filename, SampleRate, &impl->n_samples, &ret);

	if (ret != MYSOFA_OK) {
		pw_log_error("Unable to load HRTF from %s: %d", filename, ret);
		errno = ENOENT;
		goto error;
	}

	if (impl->blocksize <= 0)
		impl->blocksize = SPA_CLAMP(impl->n_samples, 64, 256);
	if (impl->tailsize <= 0)
		impl->tailsize = SPA_CLAMP(4096, impl->blocksize, 32768);

	pw_log_info("using n_samples:%u %d:%d blocksize sofa:%s", impl->n_samples,
		impl->blocksize, impl->tailsize, filename);

	impl->tmp[0] = calloc(MAX_SAMPLES, sizeof(float));
	impl->tmp[1] = calloc(MAX_SAMPLES, sizeof(float));
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
	if ((left_delay || right_delay) && (!isnan(left_delay) || !isnan(right_delay))) {
		pw_log_warn("delay dropped l: %f, r: %f", left_delay, right_delay);
	}

	if (impl->l_conv[2])
		convolver_free(impl->l_conv[2]);
	if (impl->r_conv[2])
		convolver_free(impl->r_conv[2]);

	impl->l_conv[2] = convolver_new(dsp_ops, impl->blocksize, impl->tailsize,
			left_ir, impl->n_samples);
	impl->r_conv[2] = convolver_new(dsp_ops, impl->blocksize, impl->tailsize,
			right_ir, impl->n_samples);

	free(left_ir);
	free(right_ir);

	if (impl->l_conv[2] == NULL || impl->r_conv[2] == NULL) {
		pw_log_error("reloading left or right convolver failed");
		return;
	}
	spa_loop_invoke(data_loop, do_switch, 1, NULL, 0, true, impl);
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
		uint32_t len = SPA_MIN(SampleCount, MAX_SAMPLES);
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

		spa_loop_invoke(main_loop, do_free, 1, &free_data, sizeof(free_data), false, impl);
	} else if (impl->l_conv[0] && impl->r_conv[0]) {
		convolver_run(impl->l_conv[0], impl->port[2], impl->port[0], SampleCount);
		convolver_run(impl->r_conv[0], impl->port[2], impl->port[1], SampleCount);
	}
}

static void spatializer_connect_port(void * Instance, unsigned long Port,
                        float * DataLocation)
{
	struct spatializer_impl *impl = Instance;
	if (Port > 5)
		return;
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
	pw_log_info("control changed");
	spatializer_reload(Instance);
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

static struct fc_port spatializer_ports[] = {
	{ .index = 0,
	  .name = "Out L",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Out R",
	  .flags = FC_PORT_OUTPUT | FC_PORT_AUDIO,
	},
	{ .index = 2,
	  .name = "In",
	  .flags = FC_PORT_INPUT | FC_PORT_AUDIO,
	},

	{ .index = 3,
	  .name = "Azimuth",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = 0.0f, .max = 360.0f
	},
	{ .index = 4,
	  .name = "Elevation",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 0.0f, .min = -90.0f, .max = 90.0f
	},
	{ .index = 5,
	  .name = "Radius",
	  .flags = FC_PORT_INPUT | FC_PORT_CONTROL,
	  .def = 1.0f, .min = 0.0f, .max = 100.0f
	},
};

static const struct fc_descriptor spatializer_desc = {
	.name = "spatializer",

	.n_ports = 6,
	.ports = spatializer_ports,

	.instantiate = spatializer_instantiate,
	.connect_port = spatializer_connect_port,
	.control_changed = spatializer_control_changed,
	.deactivate = spatializer_deactivate,
	.run = spatializer_run,
	.cleanup = spatializer_cleanup,
};

static const struct fc_descriptor * sofa_descriptor(unsigned long Index)
{
	switch(Index) {
	case 0:
		return &spatializer_desc;
	}
	return NULL;
}


static const struct fc_descriptor *sofa_make_desc(struct fc_plugin *plugin, const char *name)
{
	unsigned long i;
	for (i = 0; ;i++) {
		const struct fc_descriptor *d = sofa_descriptor(i);
		if (d == NULL)
			break;
		if (spa_streq(d->name, name))
			return d;
	}
	return NULL;
}

static struct fc_plugin builtin_plugin = {
	.make_desc = sofa_make_desc
};

SPA_EXPORT
struct fc_plugin *pipewire__filter_chain_plugin_load(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *dsp, const char *plugin, const char *config)
{
	dsp_ops = dsp;
	pffft_select_cpu(dsp->cpu_flags);

	data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);

	return &builtin_plugin;
}
