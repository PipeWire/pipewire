/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2024 Asymptotic Inc. */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/json-core.h>
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
 * \code{.unparsed}
 * Preamp: -6.8 dB
 * Filter 1: ON PK Fc 21 Hz Gain 6.7 dB Q 1.100
 * Filter 2: ON PK Fc 85 Hz Gain 6.9 dB Q 3.000
 * Filter 3: ON PK Fc 110 Hz Gain -2.6 dB Q 2.700
 * Filter 4: ON PK Fc 210 Hz Gain 5.9 dB Q 2.100
 * Filter 5: ON PK Fc 710 Hz Gain -1.0 dB Q 0.600
 * Filter 6: ON PK Fc 1600 Hz Gain 2.3 dB Q 2.700
 * \endcode
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
 * - `remote.name = <str>`: environment with remote name, default "pipewire-0"
 * - `capture.props = {}`: properties passed to the input stream, default `{ media.class = "Audio/Sink", node.name = "effect_input.eq<number of nodes>" }`
 * - `playback.props = {}`: properties passed to the output stream, default `{ node.passive = true, node.name = "effect_output.eq<number of nodes>" }`
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
 *         #capture.props = {
 *         #  node.name = "Parametric EQ input"
 *         #}
 *         #playback.props = {
 *         #  node.name = "Parametric EQ output"
 *         #}
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
			"( audio.channels=<number of channels> )"	\
			"( audio.position=<channel map> )"		\
			"( capture.props=<properties> )"		\
			"( playback.props=<properties> )"

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

static int enhance_properties(struct pw_properties *props, const char *key, ...)
{
	FILE *f;
	spa_autoptr(pw_properties) p = NULL;
	char *args = NULL;
	const char *str;
	size_t size;
        va_list varargs;
	int res;

	if ((str = pw_properties_get(props, key)) == NULL)
		str = "{}";
	if ((p = pw_properties_new_string(str)) == NULL)
		return -errno;

	va_start(varargs, key);
        while (true) {
		char *k, *v;
                k = va_arg(varargs, char *);
		if (k == NULL)
			break;
                v = va_arg(varargs, char *);
		if (v == NULL || pw_properties_get(p, k) == NULL)
			pw_properties_set(p, k, v);
        }
        va_end(varargs);

	if ((f = open_memstream(&args, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		return res;
	}
	pw_properties_serialize_dict(f, &p->dict, PW_PROPERTIES_FLAG_ENCLOSE);
	fclose(f);

	pw_properties_set(props, key, args);
	free(args);
	return 0;
}

static int create_eq_filter(struct impl *impl, const char *filename)
{
	FILE *f = NULL;
	const char* str;
	char *args = NULL;
	size_t size;
	int32_t res = 0;
	char path[PATH_MAX];

	if ((str = pw_properties_get(impl->props, "equalizer.description")) != NULL) {
		if (pw_properties_get(impl->props, PW_KEY_NODE_DESCRIPTION) == NULL)
			pw_properties_set(impl->props, PW_KEY_NODE_DESCRIPTION, str);
		if (pw_properties_get(impl->props, PW_KEY_MEDIA_NAME) == NULL)
			pw_properties_set(impl->props, PW_KEY_MEDIA_NAME, str);
	}

	spa_json_encode_string(path, sizeof(path), filename);
	pw_properties_setf(impl->props, "filter.graph",
			"{"
			"  nodes = [ "
			"    { type = builtin name = eq label = param_eq "
			"      config = { filename = %s } "
			"    } "
			"  ] "
			"}", path);

	enhance_properties(impl->props, "capture.props", PW_KEY_MEDIA_CLASS, "Audio/Sink", NULL);
	enhance_properties(impl->props, "playback.props", PW_KEY_NODE_PASSIVE, "true", NULL);

	if ((f = open_memstream(&args, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		goto done;
	}
	pw_properties_serialize_dict(f, &impl->props->dict, PW_PROPERTIES_FLAG_ENCLOSE);
	fclose(f);

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
	if (impl->eq_module)
		pw_impl_module_destroy(impl->eq_module);

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

	if ((str = pw_properties_get(props, "equalizer.filepath")) == NULL) {
		res = -ENOENT;
		pw_log_error( "missing property equalizer.filepath: %s", spa_strerror(res));
		goto error;
	}

	if ((res = create_eq_filter(impl, str)) < 0) {
		pw_log_error("failed to parse equalizer file: %s", spa_strerror(res));
		goto error;
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
