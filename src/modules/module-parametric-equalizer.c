/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2024 Asymptotic Inc. */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>

/** \page page_module_parametric_equalizer Parametric-Equalizer
 *
 * The `parametric-equalizer` module loads parametric equalizer configuration
 * generated from the AutoEQ project or Squiglink. Both the projects allow
 * equalizing headphones or an in-ear monitor to a target curve. While these
 * generate a file for parametric equalization for a given target, but this
 * is not a format that can be directly given to filter chain module.
 *
 * A popular example of the above being EQ'ing to the Harman target curve
 * or EQ'ing one headphone/IEM to another.
 *
 * For AutoEQ, see https://github.com/jaakkopasanen/AutoEq.
 * For SquigLink, see https://squig.link/.
 *
 * Parametric equalizer configuration generated from AutoEQ or Squiglink looks
 * like below.
 *
 * Preamp: -6.8 dB
 * Filter 1: ON PK Fc 21 Hz Gain 6.7 dB Q 1.100
 * Filter 2: ON PK Fc 85 Hz Gain 6.9 dB Q 3.000
 * Filter 3: ON PK Fc 110 Hz Gain -2.6 dB Q 2.700
 * Filter 4: ON PK Fc 210 Hz Gain 5.9 dB Q 2.100
 * Filter 5: ON PK Fc 710 Hz Gain -1.0 dB Q 0.600
 * Filter 6: ON PK Fc 1600 Hz Gain 2.3 dB Q 2.700
 *
 * Fc, Gain and Q specify the frequency, gain and Q factor respectively.
 * The fourth column can be one of PK, LSC or HSC specifying peaking, low
 * shelf and high shelf filter respectively. More often than not only peaking
 * filters are involved.
 *
 * This module parses a configuration like above and loads the filter chain
 * module with the above configuration translated to filter chain arguments.
 *
 * ## Module Name
 *
 * `libpipewire-module-parametric-equalizer`
 *
 * ## Module Options
 *
 * Options specific to the behaviour of this module
 *
 * - `equalizer.filepath = <str>` path of the file with parametric EQ
 * - `equalizer.description = <str>`: Name which will show up in
 * - `audio.channels = <int>`: Number of audio channels, default 2
 * - `audio.position = <str>`: Channel map, default "[FL, FR]"
 * - `remote.name =<str>`: environment with remote name, default "pipewire-0"
 *
 * ## General options
 *
 * Options with well-known behaviour:
 *
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_REMOTE_NAME
 *
 * ## Example configuration
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-parametric-equalizer.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-parametric-equalizer
 *     args = {
 *         #remote.name = "pipewire-0"
 *         #equalizer.filepath = "/a/b/EQ.txt"
 *         #equalizer.description = "Parametric EQ Sink"
 *         #audio.channels = 2
 *         #audio.position = [FL, FR]
 *     }
 * }
 * ]
 *\endcode
 *
 * \since 1.0.6
 */

#define NAME "parametric-eq"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_DESCRIPTION "Parametric Equalizer Sink";
#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

#define MODULE_USAGE	"( remote.name=<remote> ) "			\
			"( equalizer.filepath=<filepath> )"		\
			"( equalizer.description=<description> )"	\
			"( audio.channels=<number of channels>)"	\
			"( audio.position=<channel map>)"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Creates a module-filter-chain from Parametric EQ file" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;
	struct pw_properties *props;

	struct pw_core *core;
	struct pw_impl_module *module;
	struct pw_impl_module *eq_module;

	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;
	struct spa_hook module_listener;
	struct spa_hook eq_module_listener;

	char position[64];
	uint32_t channels;
	unsigned int do_disconnect:1;
};

struct eq_node_param {
	char filter_type[4];
	char filter[4];
	uint32_t freq;
	float gain;
	float q_fact;
};

static void filter_chain_module_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->eq_module_listener);
	impl->eq_module = NULL;
}

static const struct pw_impl_module_events filter_chain_module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = filter_chain_module_destroy,
};

void init_eq_node(FILE *f, const char *node_desc) {
	fprintf(f, "{\n");
	fprintf(f, "node.description = \"%s\"\n", node_desc);
	fprintf(f, "media.name = \"%s\"\n", node_desc);
	fprintf(f, "filter.graph = {\n");
	fprintf(f, "nodes = [\n");
}

void add_eq_node(FILE *f, struct eq_node_param *param, uint32_t eq_band_idx) {
	fprintf(f, "{\n");
	fprintf(f, "type = builtin\n");
	fprintf(f, "name = eq_band_%d\n", eq_band_idx);

	if (strcmp(param->filter_type, "PK") == 0) {
		fprintf(f, "label = bq_peaking\n");
	} else if (strcmp(param->filter_type, "LSC") == 0) {
		fprintf(f, "label = bq_lowshelf\n");
	} else if (strcmp(param->filter_type, "HSC") == 0) {
		fprintf(f, "label = bq_highshelf\n");
	} else {
		fprintf(f, "label = bq_peaking\n");
	}

	fprintf(f, "control = { \"Freq\" = %d \"Q\" = %f \"Gain\" = %f }\n", param->freq, param->q_fact, param->gain);

	fprintf(f, "}\n");
}

void end_eq_node(struct impl *impl, FILE *f, uint32_t number_of_nodes) {
	fprintf(f, "]\n");

	fprintf(f, "links = [\n");
	for (uint32_t i = 1; i < number_of_nodes; i++) {
		fprintf(f, "{ output = \"eq_band_%d:Out\" input = \"eq_band_%d:In\" }\n", i, i + 1);
	}
	fprintf(f, "]\n");

	fprintf(f, "}\n");
	fprintf(f, "audio.channels = %d\n", impl->channels);
	fprintf(f, "audio.position = %s\n", impl->position);

	fprintf(f, "capture.props = {\n");
	fprintf(f, "node.name = \"effect_input.eq%d\"\n", number_of_nodes);
	fprintf(f, "media.class = Audio/Sink\n");
	fprintf(f, "}\n");

	fprintf(f, "playback.props = {\n");
	fprintf(f, "node.name = \"effect_output.eq%d\"\n", number_of_nodes);
	fprintf(f, "node.passive = true\n");
	fprintf(f, "}\n");

	fprintf(f, "}\n");
}

int32_t parse_eq_filter_file(struct impl *impl, FILE *f)
{
	struct eq_node_param eq_param;
	FILE *memstream = NULL;
	const char* str;

	char *args = NULL;
	char *line = NULL;
	ssize_t nread;
	size_t len, size;
	uint32_t eq_band_idx = 1;
	uint32_t eq_bands = 0;
	int32_t res = 0;

	if ((memstream = open_memstream(&args, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		goto done;
	}

	if ((str = pw_properties_get(impl->props, "equalizer.description")) == NULL)
		str = DEFAULT_DESCRIPTION;
	init_eq_node(memstream, str);

	/*
	 * Read the Preamp gain line.
	 * Example: Preamp: -6.8 dB
	 *
	 * When a pre-amp gain is required, which is usually the case when
	 * applying EQ, we need to modify the first EQ band to apply a
	 * bq_highshelf filter at frequency 0 Hz with the provided negative
	 * gain.
	 *
	 * Pre-amp gain is always negative to offset the effect of possible
	 * clipping introduced by the amplification resulting from EQ.
	 */
	spa_zero(eq_param);
	nread = getline(&line, &len, f);
	if (nread != -1 && sscanf(line, "%*s %6f %*s", &eq_param.gain) == 1) {
		memcpy(eq_param.filter, "ON", 2);
		memcpy(eq_param.filter_type, "HSC", 3);
		eq_param.freq = 0;
		eq_param.q_fact = 1.0;

		add_eq_node(memstream, &eq_param, eq_band_idx);

		eq_band_idx++;
		eq_bands++;
	}

	/* Read the filter bands */
	while ((nread = getline(&line, &len, f)) != -1) {
		spa_zero(eq_param);

		/*
		 * On field widths:
		 * - filter can be ON or OFF
		 * - filter type can be PK, LSC, HSC
		 * - freq can be at most 5 decimal digits
		 * - gain can be -xy.z
		 * - Q can be x.y00
		 *
		 * Use a field width of 6 for gain and Q to account for any
		 * possible zeros.
		 */
		if (sscanf(line, "%*s %*d: %3s %3s %*s %5d %*s %*s %6f %*s %*c %6f", eq_param.filter, eq_param.filter_type, &eq_param.freq, &eq_param.gain, &eq_param.q_fact) == 5) {
			if (strcmp(eq_param.filter, "ON") == 0) {
				add_eq_node(memstream, &eq_param, eq_band_idx);

				eq_band_idx++;
				eq_bands++;
			}
		}
	}

	if (eq_bands > 0) {
		end_eq_node(impl, memstream, eq_bands);
	} else {
		pw_log_error("failed to parse equalizer configuration");
		res = -errno;
		goto done;
	}

	fclose(memstream);
	memstream = NULL;

	pw_log_info("loading new module-filter-chain with args: %s", args);
	impl->eq_module = pw_context_load_module(impl->context,
				"libpipewire-module-filter-chain",
				args, NULL);
	if (!impl->eq_module) {
		res = -errno;
		pw_log_error("Can't load module: %m");
		goto done;
	}
	pw_log_info("loaded new module-filter-chain");

	pw_impl_module_add_listener(impl->eq_module,
			&impl->eq_module_listener,
			&filter_chain_module_events, impl);

	res = 0;

done:
	if (memstream != NULL)
		fclose(memstream);
	free(args);

	return res;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);
	pw_properties_free(impl->props);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl;
	const char *str;
	FILE *f = NULL;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->props = props;

	impl->module = module;
	impl->context = context;

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}

	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	impl->channels = pw_properties_get_uint32(impl->props, PW_KEY_AUDIO_CHANNELS, DEFAULT_CHANNELS);
	if (impl->channels == 0) {
		res = -EINVAL;
		pw_log_error("invalid channels '%d'", impl->channels);
		goto error;
	}

	if ((str = pw_properties_get(impl->props, SPA_KEY_AUDIO_POSITION)) == NULL)
		str = DEFAULT_POSITION;
	strncpy(impl->position, str, strlen(str));

	if ((str = pw_properties_get(props, "equalizer.filepath")) == NULL) {
		res = -errno;
		pw_log_error( "missing property equalizer.filepath: %m");
		goto error;
	}

	pw_log_info("Loading equalizer file %s for parsing", str);

	if ((f = fopen(str, "r")) == NULL) {
		res = -errno;
		pw_log_error("failed to open equalizer file: %m");
		goto error;
	}

	if (parse_eq_filter_file(impl, f) == -1) {
		res = -EINVAL;
		pw_log_error("failed to parse equalizer file: %m");
		goto error;
	}

	fclose(f);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	if (f != NULL)
		fclose(f);

	impl_destroy(impl);

	return res;
}
