/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>

#include "config.h"

#include "module-filter-chain/plugin.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/support/cpu.h>
#include <spa/param/latency-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/pod/dynamic.h>
#include <spa/debug/types.h>

#include <pipewire/utils.h>
#include <pipewire/impl.h>
#include <pipewire/extensions/profiler.h>

#define NAME "filter-chain"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

/**
 * \page page_module_filter_chain Filter-Chain
 *
 * The filter-chain allows you to create an arbitrary processing graph
 * from LADSPA, LV2 and builtin filters. This filter can be made into a
 * virtual sink/source or between any 2 nodes in the graph.
 *
 * The filter chain is built with 2 streams, a capture stream providing
 * the input to the filter chain and a playback stream sending out the
 * filtered stream to the next nodes in the graph.
 *
 * Because both ends of the filter-chain are built with streams, the session
 * manager can manage the configuration and connection with the sinks and
 * sources automatically.
 *
 * ## Module Name
 *
 * `libpipewire-module-filter-chain`
 *
 * ## Module Options
 *
 * - `node.description`: a human readable name for the filter chain
 * - `filter.graph = []`: a description of the filter graph to run, see below
 * - `capture.props = {}`: properties to be passed to the input stream
 * - `playback.props = {}`: properties to be passed to the output stream
 *
 * ## Filter graph description
 *
 * The general structure of the graph description is as follows:
 *
 *\code{.unparsed}
 *     filter.graph = {
 *         nodes = [
 *             {
 *                 type = <ladspa | lv2 | builtin | sofa>
 *                 name = <name>
 *                 plugin = <plugin>
 *                 label = <label>
 *                 config = {
 *                     <configkey> = <value> ...
 *                 }
 *                 control = {
 *                     <controlname|controlindex> = <value> ...
 *                 }
 *             }
 *             ...
 *         ]
 *         links = [
 *             { output = <portname> input = <portname> }
 *             ...
 *         ]
 *         inputs = [ <portname> ... ]
 *         outputs = [ <portname> ... ]
 *         capture.volumes = [
 *             { control = <portname>  min = <value>  max = <value>  scale = <scale> } ...
 *         ]
 *         playback.volumes = [
 *             { control = <portname>  min = <value>  max = <value>  scale = <scale> } ...
 *         ]
 *    }
 *\endcode
 *
 * ### Nodes
 *
 * Nodes describe the processing filters in the graph. Use a tool like lv2ls
 * or listplugins to get a list of available plugins, labels and the port names.
 *
 * - `type` is one of `ladspa`, `lv2`, `builtin` or `sofa`.
 * - `name` is the name for this node, you might need this later to refer to this node
 *    and its ports when setting controls or making links.
 * - `plugin` is the type specific plugin name.
 *    - For LADSPA plugins it will append `.so` to find the shared object with that
 *       name in the LADSPA plugin path.
 *    - For LV2, this is the plugin URI obtained with lv2ls.
 *    - For builtin and sofa this is ignored
 * - `label` is the type specific filter inside the plugin.
 *    - For LADSPA this is the label
 *    - For LV2 this is unused
 *    - For builtin and sofa this is the name of the filter to use
 *
 * - `config` contains a filter specific configuration section. Some plugins need
 *            this. (convolver, sofa, delay, ...)
 * - `control` contains the initial values for the control ports of the filter.
 *            normally these are given with the port name but it is also possible
 *            to give the control index as the key.
 *
 * ### Links
 *
 * Links can be made between ports of nodes. The `portname` is given as
 * `<node_name>:<port_name>`.
 *
 * You can tee the output of filters to multiple other filters. You need to
 * use a mixer if you want the output of multiple filters to go into one
 * filter input port.
 *
 * links can be omited when the graph has just 1 filter.
 *
 * ### Inputs and Outputs
 *
 * These are the entry and exit ports into the graph definition. Their number
 * defines the number of channels used by the filter-chain.
 *
 * The `<portname>` can be `null` when a channel is to be ignored.
 *
 * Each input/output in the graph can only be linked to one filter input/output.
 * You need to use the copy builtin filter if the stream signal needs to be routed
 * to multiple filters. You need to use the mixer builtin plugin if multiple graph
 * outputs need to go to one output stream.
 *
 * inputs and outputs can be omitted, in which case the filter-chain will use all
 * inputs from the first filter and all outputs from the last filter node. The
 * graph will then be duplicated as many times to match the number of input/output
 * channels of the streams.
 *
 * ### Volumes
 *
 * Normally the volume of the sink/source is handled by the stream software volume.
 * With the capture.volumes and playback.volumes properties this can be handled
 * by a control port in the graph instead. Use capture.volumes for the volume of the
 * input of the filter (when for example used as a sink). Use playback,volumes for
 * the volume of the output of the filter (when for example used as a source).
 *
 * The min and max values (defaults 0.0 and 1.0) respectively can be used to scale
 * and translate the volume min and max values.
 *
 * Normally the control values are linear and it is assumed that the plugin does not
 * perform any scaling to the values. This can be changed with the scale property. By
 * default this is linear but it can be set to cubic when the control applies a
 * cubic transformation.
 *
 * ## Builtin filters
 *
 * There are some useful builtin filters available. You select them with the label
 * of the filter node.
 *
 * ### Mixer
 *
 * Use the `mixer` plugin if you have multiple input signals that need to be mixed together.
 *
 * The mixer plugin has up to 8 input ports labeled "In 1" to "In 8" and each with
 * a gain control labeled "Gain 1" to "Gain 8". There is an output port labeled
 * "Out". Unused input ports will be ignored and not cause overhead.
 *
 * ### Copy
 *
 * Use the `copy` plugin if you need to copy a stream input signal to multiple filters.
 *
 * It has one input port "In" and one output port "Out".
 *
 * ### Biquads
 *
 * Biquads can be used to do all kinds of filtering. They are also used when creating
 * equalizers.
 *
 * All biquad filters have an input port "In" and an output port "Out". They have
 * a "Freq", "Q" and "Gain" control. Their meaning depends on the particular biquad that
 * is used. The biquads also have "b0", "b1", "b2", "a0", "a1" and "a2" ports that
 * are read-only except for the bq_raw biquad, which can configure default values
 * depending on the graph rate and change those at runtime.
 *
 * We refer to https://arachnoid.com/BiQuadDesigner/index.html for an explanation of
 * the controls.
 *
 * The following labels can be used:
 *
 * - `bq_lowpass` a lowpass filter.
 * - `bq_highpass` a highpass filter.
 * - `bq_bandpass` a bandpass filter.
 * - `bq_lowshelf` a low shelf filter.
 * - `bq_highshelf` a high shelf filter.
 * - `bq_peaking` a peaking filter.
 * - `bq_notch` a notch filter.
 * - `bq_allpass` an allpass filter.
 * - `bq_raw` a raw biquad filter. You need a config section to specify coefficients
 *		per sample rate. The coefficients of the sample rate closest to the
 *		graph rate are selected:
 *
 *\code{.unparsed}
 * filter.graph = {
 *     nodes = [
 *         {
 *             type   = builtin
 *             name   = ...
 *             label  = bq_raw
 *             config = {
 *                 coefficients = [
 *                     { rate =  44100, b0=.., b1=.., b2=.., a0=.., a1=.., a2=.. },
 *                     { rate =  48000, b0=.., b1=.., b2=.., a0=.., a1=.., a2=.. },
 *                     { rate = 192000, b0=.., b1=.., b2=.., a0=.., a1=.., a2=.. }
 *                 ]
 *             }
 *             ...
 *         }
 *     }
 *     ...
 * }
 *\endcode
 *
 * ### Convolver
 *
 * The convolver can be used to apply an impulse response to a signal. It is usually used
 * for reverbs or virtual surround. The convolver is implemented with a fast FFT
 * implementation.
 *
 * The convolver has an input port "In" and an output port "Out". It requires a config
 * section in the node declaration in this format:
 *
 *\code{.unparsed}
 * filter.graph = {
 *     nodes = [
 *         {
 *             type   = builtin
 *             name   = ...
 *             label  = convolver
 *             config = {
 *                 blocksize = ...
 *                 tailsize = ...
 *                 gain = ...
 *                 delay = ...
 *                 filename = ...
 *                 offset = ...
 *                 length = ...
 *                 channel = ...
 *                 resample_quality = ...
 *             }
 *             ...
 *         }
 *     }
 *     ...
 * }
 *\endcode
 *
 * - `blocksize` specifies the size of the blocks to use in the FFT. It is a value
 *               between 64 and 256. When not specified, this value is
 *               computed automatically from the number of samples in the file.
 * - `tailsize` specifies the size of the tail blocks to use in the FFT.
 * - `gain`     the overall gain to apply to the IR file.
 * - `delay`    The extra delay (in samples) to add to the IR.
 * - `filename` The IR to load or create. Possible values are:
 *     - `/hilbert` creates a [hilbert function](https://en.wikipedia.org/wiki/Hilbert_transform)
 *                that can be used to phase shift the signal by +/-90 degrees. The
 *                `length` will be used as the number of coefficients.
 *     - `/dirac` creates a [Dirac function](https://en.wikipedia.org/wiki/Dirac_delta_function) that
 *                 can be used as gain.
 *     - A filename to load as the IR. This needs to be a file format supported
 *               by sndfile.
 *     - [ filename, ... ] an array of filenames. The file with the closest samplerate match
 *               with the graph samplerate will be used.
 * - `offset`  The sample offset in the file as the start of the IR.
 * - `length`  The number of samples to use as the IR.
 * - `channel` The channel to use from the file as the IR.
 * - `resample_quality` The resample quality in case the IR does not match the graph
 *                      samplerate.
 *
 * ### Delay
 *
 * The delay can be used to delay a signal in time.
 *
 * The delay has an input port "In" and an output port "Out". It also has
 * a "Delay (s)" control port. It requires a config section in the node declaration
 * in this format:
 *
 *\code{.unparsed}
 * filter.graph = {
 *     nodes = [
 *         {
 *             type   = builtin
 *             name   = ...
 *             label  = delay
 *             config = {
 *                 "max-delay" = ...
 *             }
 *             control = {
 *                 "Delay (s)" = ...
 *             }
 *             ...
 *         }
 *     }
 *     ...
 * }
 *\endcode
 *
 * - `max-delay` the maximum delay in seconds. The "Delay (s)" parameter will
 *              be clamped to this value.
 *
 * ### Invert
 *
 * The invert plugin can be used to invert the phase of the signal.
 *
 * It has an input port "In" and an output port "Out".
 *
 * ### Clamp
 *
 * The clamp plugin can be used to clamp samples between min and max values.
 *
 * It has an input port "In" and an output port "Out". It also has a "Control"
 * and "Notify" port for the control values.
 *
 * The final result is clamped to the "Min" and "Max" control values.
 *
 * ### Linear
 *
 * The linear plugin can be used to apply a linear transformation on samples
 * or control values.
 *
 * It has an input port "In" and an output port "Out". It also has a "Control"
 * and "Notify" port for the control values.
 *
 * The control value "Mult" and "Add" are used to configure the linear transform. Each
 * sample or control value will be calculated as: new = old * Mult + Add.
 *
 * ### Reciprocal
 *
 * The recip plugin can be used to calculate the reciprocal (1/x) of samples
 * or control values.
 *
 * It has an input port "In" and an output port "Out". It also has a "Control"
 * and "Notify" port for the control values.
 *
 * ### Exp
 *
 * The exp plugin can be used to calculate the exponential (base^x) of samples
 * or control values.
 *
 * It has an input port "In" and an output port "Out". It also has a "Control"
 * and "Notify" port for the control values.
 *
 * The control value "Base" is used to calculate base ^ x for each sample.
 *
 * ### Log
 *
 * The log plugin can be used to calculate the logarithm of samples
 * or control values.
 *
 * It has an input port "In" and an output port "Out". It also has a "Control"
 * and "Notify" port for the control values.
 *
 * The control value "Base", "M1" and "M2" are used to calculate
 * out = M2 * log2f(fabsf(in * M1)) / log2f(Base) for each sample.
 *
 * ### Multiply
 *
 * The mult plugin can be used to multiply samples together.
 *
 * It has 8 input ports named "In 1" to "In 8" and an output port "Out".
 *
 * All input ports samples are multiplied together into the output. Unused input ports
 * will be ignored and not cause overhead.
 *
 * ### Sine
 *
 * The sine plugin generates a sine wave.
 *
 * It has an output port "Out" and also a control output port "notify".
 *
 * "Freq", "Ampl", "Offset" and "Phase" can be used to control the sine wave
 * frequence, amplitude, offset and phase.
 *
 * ## SOFA filter
 *
 * There is an optional builtin SOFA filter available.
 *
 * ### Spatializer
 *
 * The spatializer can be used to place the sound in a 3D space.
 *
 * The spatializer has an input port "In" and a stereo pair of output ports
 * called "Out L" and "Out R". It requires a config section in the node
 * declaration in this format:
 *
 * The control can be changed at runtime to move the sounds around in the
 * 3D space.
 *
 *\code{.unparsed}
 * filter.graph = {
 *     nodes = [
 *         {
 *             type   = sofa
 *             name   = ...
 *             label  = spatializer
 *             config = {
 *                 blocksize = ...
 *                 tailsize = ...
 *                 filename = ...
 *             }
 *             control = {
 *                 "Azimuth" = ...
 *                 "Elevation" = ...
 *                 "Radius" = ...
 *             }
 *             ...
 *         }
 *     }
 *     ...
 * }
 *\endcode
 *
 * - `blocksize` specifies the size of the blocks to use in the FFT. It is a value
 *               between 64 and 256. When not specified, this value is
 *               computed automatically from the number of samples in the file.
 * - `tailsize` specifies the size of the tail blocks to use in the FFT.
 * - `filename` The SOFA file to load. SOFA files usually end in the .sofa extension
 *              and contain the HRTF for the various spatial positions.
 *
 * - `Azimuth`   controls the azimuth, this is the direction the sound is coming from
 *               in degrees between 0 and 360. 0 is straight ahead. 90 is left, 180
 *               behind, 270 right.
 * - `Elevation` controls the elevation, this is how high/low the signal is in degrees
 *               between -90 and 90. 0 is straight in front, 90 is directly above
 *               and -90 directly below.
 * - `Radius`    controls how far away the signal is as a value between 0 and 100.
 *               default is 1.0.
 *
 * ## General options
 *
 * Options with well-known behavior. Most options can be added to the global
 * configuration or the individual streams:
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_MEDIA_NAME
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LINK_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_NODE_NAME: See notes below. If not specified, defaults to
 *	'filter-chain-<pid>-<module-id>'.
 *
 * Stream only properties:
 *
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_NODE_NAME:  if not given per stream, the global node.name will be
 *         prefixed with 'input.' and 'output.' to generate a capture and playback
 *         stream node.name respectively.
 *
 * ## Example configuration of a virtual source
 *
 * This example uses the rnnoise LADSPA plugin to create a new
 * virtual source.
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-filter-chain
 *     args = {
 *         node.description =  "Noise Canceling source"
 *         media.name =  "Noise Canceling source"
 *         filter.graph = {
 *             nodes = [
 *                 {
 *                     type = ladspa
 *                     name = rnnoise
 *                     plugin = ladspa/librnnoise_ladspa
 *                     label = noise_suppressor_stereo
 *                     control = {
 *                         "VAD Threshold (%)" 50.0
 *                     }
 *                 }
 *             ]
 *         }
 *         capture.props = {
 *             node.name =  "capture.rnnoise_source"
 *             node.passive = true
 *         }
 *         playback.props = {
 *             node.name =  "rnnoise_source"
 *             media.class = Audio/Source
 *         }
 *     }
 * }
 * ]
 *\endcode
 *
 * ## Example configuration of a Dolby Surround encoder virtual Sink
 *
 * This example uses the ladpsa surround encoder to encode a 5.1 signal
 * to a stereo Dolby Surround signal.
 *
 *\code{.unparsed}
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-filter-chain
 *     args = {
 *         node.description = "Dolby Surround Sink"
 *         media.name       = "Dolby Surround Sink"
 *         filter.graph = {
 *             nodes = [
 *                 {
 *                     type  = builtin
 *                     name  = mixer
 *                     label = mixer
 *                     control = { "Gain 1" = 0.5 "Gain 2" = 0.5 }
 *                 }
 *                 {
 *                     type   = ladspa
 *                     name   = enc
 *                     plugin = surround_encoder_1401
 *                     label  = surroundEncoder
 *                 }
 *             ]
 *             links = [
 *                 { output = "mixer:Out" input = "enc:S" }
 *             ]
 *             inputs  = [ "enc:L" "enc:R" "enc:C" null "mixer:In 1" "mixer:In 2" ]
 *             outputs = [ "enc:Lt" "enc:Rt" ]
 *         }
 *         capture.props = {
 *             node.name      = "effect_input.dolby_surround"
 *             media.class    = Audio/Sink
 *             audio.channels = 6
 *             audio.position = [ FL FR FC LFE SL SR ]
 *         }
 *         playback.props = {
 *             node.name      = "effect_output.dolby_surround"
 *             node.passive   = true
 *             audio.channels = 2
 *             audio.position = [ FL FR ]
 *         }
 *     }
 * }
 * ]
 *\endcode
 */
static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create filter chain streams" },
	{ PW_KEY_MODULE_USAGE, " ( remote.name=<remote> ) "
				"( node.latency=<latency as fraction> ) "
				"( node.description=<description of the nodes> ) "
				"( audio.rate=<sample rate> ) "
				"( audio.channels=<number of channels> ) "
				"( audio.position=<channel map> ) "
				"filter.graph = [ "
				"    nodes = [ "
				"        { "
				"          type = <ladspa | lv2 | builtin | sofa> "
				"          name = <name> "
				"          plugin = <plugin> "
				"          label = <label> "
				"          config = { "
				"             <configkey> = <value> ... "
				"          } "
				"          control = { "
				"             <controlname|controlindex> = <value> ... "
				"          } "
				"        } "
				"    ] "
				"    links = [ "
				"        { output = <portname> input = <portname> } ... "
				"    ] "
				"    inputs = [ <portname> ... ] "
				"    outputs = [ <portname> ... ] "
				"] "
				"( capture.props=<properties> ) "
				"( playback.props=<properties> ) " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>

#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/pipewire.h>

#define MAX_HNDL 64

#define DEFAULT_RATE	48000

struct fc_plugin *load_ladspa_plugin(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *dsp, const char *path, const char *config);
struct fc_plugin *load_builtin_plugin(const struct spa_support *support, uint32_t n_support,
		struct dsp_ops *dsp, const char *path, const char *config);

struct plugin {
	struct spa_list link;
	int ref;
	char type[256];
	char path[PATH_MAX];

	struct fc_plugin *plugin;
	struct spa_list descriptor_list;
};

struct plugin_func {
	struct spa_list link;
	char type[256];
	fc_plugin_load_func *func;
	void *hndl;
};

struct descriptor {
	struct spa_list link;
	int ref;
	struct plugin *plugin;
	char label[256];

	const struct fc_descriptor *desc;

	uint32_t n_input;
	uint32_t n_output;
	uint32_t n_control;
	uint32_t n_notify;
	unsigned long *input;
	unsigned long *output;
	unsigned long *control;
	unsigned long *notify;
	float *default_control;
};

struct port {
	struct spa_list link;
	struct node *node;

	uint32_t idx;
	unsigned long p;

	struct spa_list link_list;
	uint32_t n_links;
	uint32_t external;

	float control_data[MAX_HNDL];
	float *audio_data[MAX_HNDL];
};

struct node {
	struct spa_list link;
	struct graph *graph;

	struct descriptor *desc;

	char name[256];
	char *config;

	struct port *input_port;
	struct port *output_port;
	struct port *control_port;
	struct port *notify_port;

	uint32_t n_hndl;
	void *hndl[MAX_HNDL];

	unsigned int n_deps;
	unsigned int visited:1;
	unsigned int disabled:1;
	unsigned int control_changed:1;
};

struct link {
	struct spa_list link;

	struct spa_list input_link;
	struct spa_list output_link;

	struct port *output;
	struct port *input;
};

struct graph_port {
	const struct fc_descriptor *desc;
	void **hndl;
	uint32_t port;
	unsigned next:1;
};

struct graph_hndl {
	const struct fc_descriptor *desc;
	void **hndl;
};

struct volume {
	bool mute;
	uint32_t n_volumes;
	float volumes[SPA_AUDIO_MAX_CHANNELS];

	uint32_t n_ports;
	struct port *ports[SPA_AUDIO_MAX_CHANNELS];
	float min[SPA_AUDIO_MAX_CHANNELS];
	float max[SPA_AUDIO_MAX_CHANNELS];
#define SCALE_LINEAR	0
#define SCALE_CUBIC	1
	int scale[SPA_AUDIO_MAX_CHANNELS];
};

struct graph {
	struct impl *impl;

	struct spa_list node_list;
	struct spa_list link_list;

	uint32_t n_input;
	struct graph_port *input;

	uint32_t n_output;
	struct graph_port *output;

	uint32_t n_hndl;
	struct graph_hndl *hndl;

	uint32_t n_control;
	struct port **control_port;

	struct volume capture_volume;
	struct volume playback_volume;

	unsigned instantiated:1;
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	uint32_t quantum_limit;
	struct dsp_ops dsp;

	struct spa_list plugin_list;
	struct spa_list plugin_func_list;

	struct pw_properties *capture_props;
	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct spa_audio_info_raw capture_info;

	struct pw_properties *playback_props;
	struct pw_stream *playback;
	struct spa_hook playback_listener;
	struct spa_audio_info_raw playback_info;

	struct spa_audio_info_raw info;

	struct spa_io_position *position;

	unsigned int do_disconnect:1;

	long unsigned rate;

	struct graph graph;

	float *silence_data;
	float *discard_data;
};

static int graph_instantiate(struct graph *graph);
static void graph_cleanup(struct graph *graph);


static void capture_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->capture_listener);
	impl->capture = NULL;
}

static void capture_process(void *d)
{
	struct impl *impl = d;
	pw_stream_trigger_process(impl->playback);
}

static void playback_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	struct graph *graph = &impl->graph;
	uint32_t i, j, insize = 0, outsize = 0, n_hndl = graph->n_hndl;
	int32_t stride = 0;
	struct graph_port *port;
	struct spa_data *bd;

	in = NULL;
	while (true) {
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(impl->capture)) == NULL)
			break;
		if (in)
			pw_stream_queue_buffer(impl->capture, in);
		in = t;
	}
	if (in == NULL)
		pw_log_debug("%p: out of capture buffers: %m", impl);

	if ((out = pw_stream_dequeue_buffer(impl->playback)) == NULL)
		pw_log_debug("%p: out of playback buffers: %m", impl);

	if (in == NULL || out == NULL)
		goto done;

	for (i = 0, j = 0; i < in->buffer->n_datas; i++) {
		uint32_t offs, size;

		bd = &in->buffer->datas[i];

		offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
		size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);

		while (j < graph->n_input) {
			port = &graph->input[j++];
			if (port->desc)
				port->desc->connect_port(*port->hndl, port->port,
					SPA_PTROFF(bd->data, offs, void));
			if (!port->next)
				break;

		}
		insize = i == 0 ? size : SPA_MIN(insize, size);
		stride = SPA_MAX(stride, bd->chunk->stride);
	}
	outsize = insize;

	for (i = 0; i < out->buffer->n_datas; i++) {
		bd = &out->buffer->datas[i];

		outsize = SPA_MIN(outsize, bd->maxsize);

		port = i < graph->n_output ? &graph->output[i] : NULL;

		if (port && port->desc)
			port->desc->connect_port(*port->hndl, port->port, bd->data);
		else
			memset(bd->data, 0, outsize);

		bd->chunk->offset = 0;
		bd->chunk->size = outsize;
		bd->chunk->stride = stride;
	}

	pw_log_trace_fp("%p: stride:%d in:%d out:%d requested:%"PRIu64" (%"PRIu64")", impl,
			stride, insize, outsize, out->requested, out->requested * stride);

	for (i = 0; i < n_hndl; i++) {
		struct graph_hndl *hndl = &graph->hndl[i];
		hndl->desc->run(*hndl->hndl, outsize / sizeof(float));
	}

done:
	if (in != NULL)
		pw_stream_queue_buffer(impl->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(impl->playback, out);
}

static float get_default(struct impl *impl, struct descriptor *desc, uint32_t p)
{
	struct fc_port *port = &desc->desc->ports[p];
	return port->def;
}

static struct node *find_node(struct graph *graph, const char *name)
{
	struct node *node;
	spa_list_for_each(node, &graph->node_list, link) {
		if (spa_streq(node->name, name))
			return node;
	}
	return NULL;
}

/* find a port by name. Valid syntax is:
 *  "<node_name>:<port_name>"
 *  "<node_name>:<port_id>"
 *  "<port_name>"
 *  "<port_id>"
 *  When no node_name is given, the port is assumed in the current node.  */
static struct port *find_port(struct node *node, const char *name, int descriptor)
{
	char *col, *node_name, *port_name, *str;
	struct port *ports;
	const struct fc_descriptor *d;
	uint32_t i, n_ports, port_id = SPA_ID_INVALID;

	str = strdupa(name);
	col = strchr(str, ':');
	if (col != NULL) {
		struct node *find;
		node_name = str;
		port_name = col + 1;
		*col = '\0';
		find = find_node(node->graph, node_name);
		if (find == NULL) {
			/* it's possible that the : is part of the port name,
			 * try again without splitting things up. */
			*col = ':';
			col = NULL;
		} else {
			node = find;
		}
	}
	if (col == NULL) {
		node_name = node->name;
		port_name = str;
	}
	if (node == NULL)
		return NULL;

	if (!spa_atou32(port_name, &port_id, 0))
		port_id = SPA_ID_INVALID;

	if (FC_IS_PORT_INPUT(descriptor)) {
		if (FC_IS_PORT_CONTROL(descriptor)) {
			ports = node->control_port;
			n_ports = node->desc->n_control;
		} else {
			ports = node->input_port;
			n_ports = node->desc->n_input;
		}
	} else if (FC_IS_PORT_OUTPUT(descriptor)) {
		if (FC_IS_PORT_CONTROL(descriptor)) {
			ports = node->notify_port;
			n_ports = node->desc->n_notify;
		} else {
			ports = node->output_port;
			n_ports = node->desc->n_output;
		}
	} else
		return NULL;

	d = node->desc->desc;
	for (i = 0; i < n_ports; i++) {
		struct port *port = &ports[i];
		if (i == port_id ||
		    spa_streq(d->ports[port->p].name, port_name))
			return port;
	}
	return NULL;
}

static struct spa_pod *get_prop_info(struct graph *graph, struct spa_pod_builder *b, uint32_t idx)
{
	struct impl *impl = graph->impl;
	struct spa_pod_frame f[2];
	struct port *port = graph->control_port[idx];
	struct node *node = port->node;
	struct descriptor *desc = node->desc;
	const struct fc_descriptor *d = desc->desc;
	struct fc_port *p = &d->ports[port->p];
	float def, min, max;
	char name[512];
	uint32_t rate = impl->rate ? impl->rate : DEFAULT_RATE;

	if (p->hint & FC_HINT_SAMPLE_RATE) {
		def = p->def * rate;
		min = p->min * rate;
		max = p->max * rate;
	} else {
		def = p->def;
		min = p->min;
		max = p->max;
	}

	if (node->name[0] != '\0')
		snprintf(name, sizeof(name), "%s:%s", node->name, p->name);
	else
		snprintf(name, sizeof(name), "%s", p->name);

	spa_pod_builder_push_object(b, &f[0],
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add (b,
			SPA_PROP_INFO_name, SPA_POD_String(name),
			0);
	spa_pod_builder_prop(b, SPA_PROP_INFO_type, 0);
	if (p->hint & FC_HINT_BOOLEAN) {
		if (min == max) {
			spa_pod_builder_bool(b, def <= 0.0f ? false : true);
		} else  {
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
			spa_pod_builder_bool(b, def <= 0.0f ? false : true);
			spa_pod_builder_bool(b, false);
			spa_pod_builder_bool(b, true);
			spa_pod_builder_pop(b, &f[1]);
		}
	} else if (p->hint & FC_HINT_INTEGER) {
		if (min == max) {
			spa_pod_builder_int(b, def);
		} else {
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Range, 0);
			spa_pod_builder_int(b, def);
			spa_pod_builder_int(b, min);
			spa_pod_builder_int(b, max);
			spa_pod_builder_pop(b, &f[1]);
		}
	} else {
		if (min == max) {
			spa_pod_builder_float(b, def);
		} else {
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Range, 0);
			spa_pod_builder_float(b, def);
			spa_pod_builder_float(b, min);
			spa_pod_builder_float(b, max);
			spa_pod_builder_pop(b, &f[1]);
		}
	}
	spa_pod_builder_prop(b, SPA_PROP_INFO_params, 0);
	spa_pod_builder_bool(b, true);
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *get_props_param(struct graph *graph, struct spa_pod_builder *b)
{
	struct spa_pod_frame f[2];
	uint32_t i;
	char name[512];

	spa_pod_builder_push_object(b, &f[0],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(b, &f[1]);

	for (i = 0; i < graph->n_control; i++) {
		struct port *port = graph->control_port[i];
		struct node *node = port->node;
		struct descriptor *desc = node->desc;
		const struct fc_descriptor *d = desc->desc;
		struct fc_port *p = &d->ports[port->p];

		if (node->name[0] != '\0')
			snprintf(name, sizeof(name), "%s:%s", node->name, p->name);
		else
			snprintf(name, sizeof(name), "%s", p->name);

		spa_pod_builder_string(b, name);
		if (p->hint & FC_HINT_BOOLEAN) {
			spa_pod_builder_bool(b, port->control_data[0] <= 0.0f ? false : true);
		} else if (p->hint & FC_HINT_INTEGER) {
			spa_pod_builder_int(b, port->control_data[0]);
		} else {
			spa_pod_builder_float(b, port->control_data[0]);
		}
	}
	spa_pod_builder_pop(b, &f[1]);
	return spa_pod_builder_pop(b, &f[0]);
}

static int port_set_control_value(struct port *port, float *value, uint32_t id)
{
	struct node *node = port->node;
	struct descriptor *desc = node->desc;
	float old;

	old = port->control_data[id];
	port->control_data[id] = value ? *value : desc->default_control[port->idx];
	pw_log_info("control %d %d ('%s') from %f to %f", port->idx, id,
			desc->desc->ports[port->p].name, old, port->control_data[id]);
	node->control_changed = old != port->control_data[id];
	return node->control_changed ? 1 : 0;
}

static int set_control_value(struct node *node, const char *name, float *value)
{
	struct port *port;
	int count = 0;
	uint32_t i, n_hndl;

	port = find_port(node, name, FC_PORT_INPUT | FC_PORT_CONTROL);
	if (port == NULL)
		return -ENOENT;

	/* if we don't have any instances yet, set the first control value, we will
	 * copy to other instances later */
	n_hndl = SPA_MAX(1u, port->node->n_hndl);
	for (i = 0; i < n_hndl; i++)
		count += port_set_control_value(port, value, i);

	return count;
}

static int parse_params(struct graph *graph, const struct spa_pod *pod)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f;
	int res, changed = 0;
	struct node *def_node;

	def_node = spa_list_first(&graph->node_list, struct node, link);

	spa_pod_parser_pod(&prs, pod);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return 0;

	while (true) {
		const char *name;
		float value, *val = NULL;
		double dbl_val;
		bool bool_val;
		int32_t int_val;

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;
		if (spa_pod_parser_get_float(&prs, &value) >= 0) {
			val = &value;
		} else if (spa_pod_parser_get_double(&prs, &dbl_val) >= 0) {
			value = dbl_val;
			val = &value;
		} else if (spa_pod_parser_get_int(&prs, &int_val) >= 0) {
			value = int_val;
			val = &value;
		} else if (spa_pod_parser_get_bool(&prs, &bool_val) >= 0) {
			value = bool_val ? 1.0f : 0.0f;
			val = &value;
		} else {
			struct spa_pod *pod;
			spa_pod_parser_get_pod(&prs, &pod);
		}
		if ((res = set_control_value(def_node, name, val)) > 0)
			changed += res;
	}
	return changed;
}

static void graph_reset(struct graph *graph)
{
	uint32_t i;
	for (i = 0; i < graph->n_hndl; i++) {
		struct graph_hndl *hndl = &graph->hndl[i];
		const struct fc_descriptor *d = hndl->desc;
		if (hndl->hndl == NULL || *hndl->hndl == NULL)
			continue;
		if (d->deactivate)
			d->deactivate(*hndl->hndl);
		if (d->activate)
			d->activate(*hndl->hndl);
	}
}

static void node_control_changed(struct node *node)
{
	const struct fc_descriptor *d = node->desc->desc;
	uint32_t i;

	if (!node->control_changed)
		return;

	for (i = 0; i < node->n_hndl; i++) {
		if (node->hndl[i] == NULL)
			continue;
		if (d->control_changed)
			d->control_changed(node->hndl[i]);
	}
	node->control_changed = false;
}

static void update_props_param(struct impl *impl)
{
	struct graph *graph = &impl->graph;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	const struct spa_pod *params[1];

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
	params[0] = get_props_param(graph, &b.b);

	pw_stream_update_params(impl->capture, params, 1);
	spa_pod_dynamic_builder_clean(&b);
}

static int sync_volume(struct graph *graph, struct volume *vol)
{
	uint32_t i;
	int res = 0;

	if (vol->n_ports == 0)
		return 0;
	for (i = 0; i < vol->n_volumes; i++) {
		uint32_t n_port = i % vol->n_ports, n_hndl;
		struct port *p = vol->ports[n_port];
		float v = vol->mute ? 0.0f : vol->volumes[i];
		switch (vol->scale[n_port]) {
		case SCALE_CUBIC:
			v = cbrt(v);
			break;
		}
		v = v * (vol->max[n_port] - vol->min[n_port]) + vol->min[n_port];

		n_hndl = SPA_MAX(1u, p->node->n_hndl);
		res += port_set_control_value(p, &v, i % n_hndl);
	}
	return res;
}

static void param_props_changed(struct impl *impl, const struct spa_pod *param,
		bool capture)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_frame f[1];
	const struct spa_pod_prop *prop;
	struct graph *graph = &impl->graph;
	int changed = 0;
	char buf[1024];
	struct spa_pod_dynamic_builder b;
	struct volume *vol = capture ? &graph->capture_volume :
		&graph->playback_volume;
	bool do_volume = false;

	spa_pod_dynamic_builder_init(&b, buf, sizeof(buf), 1024);
	spa_pod_builder_push_object(&b.b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_params:
			changed += parse_params(graph, &prop->value);
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) == 0) {
				if (vol->mute != mute) {
					vol->mute = mute;
					do_volume = true;
				}
			}
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t i, n_vols;
			float vols[SPA_AUDIO_MAX_CHANNELS];

			if ((n_vols = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols,
					SPA_AUDIO_MAX_CHANNELS)) > 0) {
				if (vol->n_volumes != n_vols)
					do_volume = true;
				vol->n_volumes = n_vols;
				for (i = 0; i < n_vols; i++) {
					float v = vols[i];
					if (v != vol->volumes[i]) {
						vol->volumes[i] = v;
						do_volume = true;
					}
				}
			}
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_softVolumes:
		case SPA_PROP_softMute:
			break;
		default:
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
	}
	if (do_volume && vol->n_ports != 0) {
		float soft_vols[SPA_AUDIO_MAX_CHANNELS];
		uint32_t i;

		for (i = 0; i < vol->n_volumes; i++)
			soft_vols[i] = (vol->mute || vol->volumes[i] == 0.0f) ? 0.0f : 1.0f;

		spa_pod_builder_prop(&b.b, SPA_PROP_softMute, 0);
		spa_pod_builder_bool(&b.b, vol->mute);
		spa_pod_builder_prop(&b.b, SPA_PROP_softVolumes, 0);
		spa_pod_builder_array(&b.b, sizeof(float), SPA_TYPE_Float,
				vol->n_volumes, soft_vols);
		param = spa_pod_builder_pop(&b.b, &f[0]);

		sync_volume(graph, vol);
		pw_stream_set_param(capture ? impl->capture :
				impl->playback, SPA_PARAM_Props, param);
	}

	spa_pod_dynamic_builder_clean(&b);

	if (changed > 0) {
		struct node *node;

		spa_list_for_each(node, &graph->node_list, link)
			node_control_changed(node);

		update_props_param(impl);
	}

}

static void param_latency_changed(struct impl *impl, const struct spa_pod *param)
{
	struct spa_latency_info latency;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];

	if (param == NULL || spa_latency_parse(param, &latency) < 0)
		return;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

	if (latency.direction == SPA_DIRECTION_INPUT)
		pw_stream_update_params(impl->capture, params, 1);
	else
		pw_stream_update_params(impl->playback, params, 1);
}

static void param_tag_changed(struct impl *impl, const struct spa_pod *param)
{
	struct spa_tag_info tag;
	const struct spa_pod *params[1] = { param };
	void *state = NULL;

	if (param == 0 || spa_tag_parse(param, &tag, &state) < 0)
		return;

	if (tag.direction == SPA_DIRECTION_INPUT)
		pw_stream_update_params(impl->capture, params, 1);
	else
		pw_stream_update_params(impl->playback, params, 1);
}

static void state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;
	struct graph *graph = &impl->graph;
	int res;

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(impl->playback, false);
		pw_stream_flush(impl->capture, false);
		graph_reset(graph);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("module %p: unconnected", impl);
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_info("module %p: error: %s", impl, error);
		break;
	case PW_STREAM_STATE_STREAMING:
	{
		uint32_t target = impl->info.rate;
		if (target == 0)
			target = impl->position ?
				impl->position->clock.target_rate.denom : DEFAULT_RATE;
		if (target == 0) {
			res = -EINVAL;
			goto error;
		}
		if (impl->rate != target) {
			impl->rate = target;
			graph_cleanup(graph);
			if ((res = graph_instantiate(graph)) < 0)
				goto error;
		}
		break;
	}
	default:
		break;
	}
	return;
error:
	pw_stream_set_error(impl->capture, res, "can't start graph: %s",
			spa_strerror(res));
}

static void io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_IO_Position:
		impl->position = area;
		break;
	default:
		break;
	}
}

static void param_changed(void *data, uint32_t id, const struct spa_pod *param,
		bool capture)
{
	struct impl *impl = data;
	struct graph *graph = &impl->graph;
	int res;

	switch (id) {
	case SPA_PARAM_Format:
	{
		struct spa_audio_info_raw info;
		spa_zero(info);
		if (param == NULL) {
			graph_cleanup(graph);
			impl->rate = 0;
		} else {
			if ((res = spa_format_audio_raw_parse(param, &info)) < 0)
				goto error;
		}
		impl->info = info;
		break;
	}
	case SPA_PARAM_Props:
		if (param != NULL)
			param_props_changed(impl, param, capture);
		break;
	case SPA_PARAM_Latency:
		param_latency_changed(impl, param);
		break;
	case SPA_PARAM_Tag:
		param_tag_changed(impl, param);
		break;
	}
	return;

error:
	pw_stream_set_error(capture ? impl->capture : impl->playback,
			res, "can't start graph: %s", spa_strerror(res));
}

static void capture_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	param_changed(data, id, param, true);
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.process = capture_process,
	.io_changed = io_changed,
	.state_changed = state_changed,
	.param_changed = capture_param_changed
};

static void playback_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	param_changed(data, id, param, false);
}

static void playback_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->playback_listener);
	impl->playback = NULL;
}

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.process = playback_process,
	.io_changed = io_changed,
	.state_changed = state_changed,
	.param_changed = playback_param_changed,
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t i, n_params, *offs;
	struct pw_array offsets;
	const struct spa_pod **params = NULL;
	struct spa_pod_dynamic_builder b;
	struct graph *graph = &impl->graph;

	impl->capture = pw_stream_new(impl->core,
			"filter capture", impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture,
			&impl->capture_listener,
			&in_stream_events, impl);

	impl->playback = pw_stream_new(impl->core,
			"filter playback", impl->playback_props);
	impl->playback_props = NULL;
	if (impl->playback == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback,
			&impl->playback_listener,
			&out_stream_events, impl);

	spa_pod_dynamic_builder_init(&b, NULL, 0, 4096);
	pw_array_init(&offsets, 512);

	if ((offs = pw_array_add(&offsets, sizeof(uint32_t))) == NULL) {
		res = -errno;
		goto done;
	}
	*offs = b.b.state.offset;
	spa_format_audio_raw_build(&b.b,
			SPA_PARAM_EnumFormat, &impl->capture_info);

	for (i = 0; i < graph->n_control; i++) {
		if ((offs = pw_array_add(&offsets, sizeof(uint32_t))) != NULL)
			*offs = b.b.state.offset;
		get_prop_info(graph, &b.b, i);
	}

	if ((offs = pw_array_add(&offsets, sizeof(uint32_t))) != NULL)
		*offs = b.b.state.offset;
	get_props_param(graph, &b.b);

	n_params = pw_array_get_len(&offsets, uint32_t);
	if (n_params == 0) {
		res = -ENOMEM;
		goto done;
	}
	if ((params = calloc(n_params, sizeof(struct spa_pod*))) == NULL) {
		res = -errno;
		goto done;
	}

	offs = offsets.data;
	for (i = 0; i < n_params; i++)
		params[i] = spa_pod_builder_deref(&b.b, offs[i]);

	res = pw_stream_connect(impl->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_ASYNC,
			params, n_params);

	spa_pod_dynamic_builder_clean(&b);
	if (res < 0)
		goto done;

	n_params = 0;
	spa_pod_dynamic_builder_init(&b, NULL, 0, 4096);
	params[n_params++] = spa_format_audio_raw_build(&b.b,
			SPA_PARAM_EnumFormat, &impl->playback_info);

	res = pw_stream_connect(impl->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS  |
			PW_STREAM_FLAG_TRIGGER,
			params, n_params);
	spa_pod_dynamic_builder_clean(&b);

done:
	free(params);
	pw_array_clear(&offsets);

	return res < 0 ? res : 0;
}

static uint32_t count_array(struct spa_json *json)
{
	struct spa_json it = *json;
	char v[256];
	uint32_t count = 0;
	while (spa_json_get_string(&it, v, sizeof(v)) > 0)
		count++;
	return count;
}

static void plugin_unref(struct plugin *hndl)
{
	if (--hndl->ref > 0)
		return;

	fc_plugin_free(hndl->plugin);

	spa_list_remove(&hndl->link);
	free(hndl);
}


static struct plugin_func *add_plugin_func(struct impl *impl, const char *type,
		fc_plugin_load_func *func, void *hndl)
{
	struct plugin_func *pl;

	pl = calloc(1, sizeof(*pl));
	if (pl == NULL)
		return NULL;

	snprintf(pl->type, sizeof(pl->type), "%s", type);
	pl->func = func;
	pl->hndl = hndl;
	spa_list_append(&impl->plugin_func_list, &pl->link);
	return pl;
}

static void free_plugin_func(struct plugin_func *pl)
{
	spa_list_remove(&pl->link);
	if (pl->hndl)
		dlclose(pl->hndl);
	free(pl);
}

static fc_plugin_load_func *find_plugin_func(struct impl *impl, const char *type)
{
	fc_plugin_load_func *func = NULL;
	void *hndl = NULL;
	int res;
	struct plugin_func *pl;
	char module[PATH_MAX];
	const char *module_dir;
	const char *state = NULL, *p;
	size_t len;

	spa_list_for_each(pl, &impl->plugin_func_list, link) {
		if (spa_streq(pl->type, type))
			return pl->func;
	}
	module_dir = getenv("PIPEWIRE_MODULE_DIR");
	if (module_dir == NULL)
		module_dir = MODULEDIR;
	pw_log_debug("moduledir set to: %s", module_dir);

	while ((p = pw_split_walk(module_dir, ":", &len, &state))) {
		if ((res = spa_scnprintf(module, sizeof(module),
				"%.*s/libpipewire-module-filter-chain-%s.so",
						(int)len, p, type)) <= 0)
			continue;

		hndl = dlopen(module, RTLD_NOW | RTLD_LOCAL);
		if (hndl != NULL)
			break;

		pw_log_debug("open plugin module %s failed: %s", module, dlerror());
	}
	if (hndl == NULL) {
		errno = ENOENT;
		return NULL;
	}
	func = dlsym(hndl, FC_PLUGIN_LOAD_FUNC);
	if (func != NULL) {
		pw_log_info("opened plugin module %s", module);
		pl = add_plugin_func(impl, type, func, hndl);
		if (pl == NULL)
			goto error_close;
	} else {
		errno = ENOSYS;
		pw_log_error("%s is not a filter chain plugin: %m", module);
		goto error_close;
	}
	return func;

error_close:
	dlclose(hndl);
	return NULL;
}

static struct plugin *plugin_load(struct impl *impl, const char *type, const char *path)
{
	struct fc_plugin *pl = NULL;
	struct plugin *hndl;
	const struct spa_support *support;
	uint32_t n_support;
	fc_plugin_load_func *plugin_func;

	spa_list_for_each(hndl, &impl->plugin_list, link) {
		if (spa_streq(hndl->type, type) &&
		    spa_streq(hndl->path, path)) {
			hndl->ref++;
			return hndl;
		}
	}
	support = pw_context_get_support(impl->context, &n_support);

	plugin_func = find_plugin_func(impl, type);
	if (plugin_func == NULL) {
		pw_log_error("can't load plugin type '%s': %m", type);
		pl = NULL;
	} else {
		pl = plugin_func(support, n_support, &impl->dsp, path, NULL);
	}
	if (pl == NULL)
		goto exit;

	hndl = calloc(1, sizeof(*hndl));
	if (!hndl)
		return NULL;

	hndl->ref = 1;
	snprintf(hndl->type, sizeof(hndl->type), "%s", type);
	snprintf(hndl->path, sizeof(hndl->path), "%s", path);

	pw_log_info("successfully opened '%s':'%s'", type, path);

	hndl->plugin = pl;

	spa_list_init(&hndl->descriptor_list);
	spa_list_append(&impl->plugin_list, &hndl->link);

	return hndl;
exit:
	return NULL;
}

static void descriptor_unref(struct descriptor *desc)
{
	if (--desc->ref > 0)
		return;

	spa_list_remove(&desc->link);
	plugin_unref(desc->plugin);
	if (desc->desc)
		fc_descriptor_free(desc->desc);
	free(desc->input);
	free(desc->output);
	free(desc->control);
	free(desc->default_control);
	free(desc->notify);
	free(desc);
}

static struct descriptor *descriptor_load(struct impl *impl, const char *type,
		const char *plugin, const char *label)
{
	struct plugin *hndl;
	struct descriptor *desc;
	const struct fc_descriptor *d;
	uint32_t i, n_input, n_output, n_control, n_notify;
	unsigned long p;
	int res;

	if ((hndl = plugin_load(impl, type, plugin)) == NULL)
		return NULL;

	spa_list_for_each(desc, &hndl->descriptor_list, link) {
		if (spa_streq(desc->label, label)) {
			desc->ref++;

			/*
			 * since ladspa_handle_load() increments the reference count of the handle,
			 * if the descriptor is found, then the handle's reference count
			 * has already been incremented to account for the descriptor,
			 * so we need to unref handle here since we're merely reusing
			 * thedescriptor, not creating a new one
			 */
			plugin_unref(hndl);
			return desc;
		}
	}

	desc = calloc(1, sizeof(*desc));
	desc->ref = 1;
	desc->plugin = hndl;
	spa_list_init(&desc->link);

	if ((d = hndl->plugin->make_desc(hndl->plugin, label)) == NULL) {
		pw_log_error("cannot find label %s", label);
		res = -ENOENT;
		goto exit;
	}
	desc->desc = d;
	snprintf(desc->label, sizeof(desc->label), "%s", label);

	n_input = n_output = n_control = n_notify = 0;
	for (p = 0; p < d->n_ports; p++) {
		struct fc_port *fp = &d->ports[p];
		if (FC_IS_PORT_AUDIO(fp->flags)) {
			if (FC_IS_PORT_INPUT(fp->flags))
				n_input++;
			else if (FC_IS_PORT_OUTPUT(fp->flags))
				n_output++;
		} else if (FC_IS_PORT_CONTROL(fp->flags)) {
			if (FC_IS_PORT_INPUT(fp->flags))
				n_control++;
			else if (FC_IS_PORT_OUTPUT(fp->flags))
				n_notify++;
		}
	}
	desc->input = calloc(n_input, sizeof(unsigned long));
	desc->output = calloc(n_output, sizeof(unsigned long));
	desc->control = calloc(n_control, sizeof(unsigned long));
	desc->default_control = calloc(n_control, sizeof(float));
	desc->notify = calloc(n_notify, sizeof(unsigned long));

	for (p = 0; p < d->n_ports; p++) {
		struct fc_port *fp = &d->ports[p];

		if (FC_IS_PORT_AUDIO(fp->flags)) {
			if (FC_IS_PORT_INPUT(fp->flags)) {
				pw_log_info("using port %lu ('%s') as input %d", p,
						fp->name, desc->n_input);
				desc->input[desc->n_input++] = p;
			}
			else if (FC_IS_PORT_OUTPUT(fp->flags)) {
				pw_log_info("using port %lu ('%s') as output %d", p,
						fp->name, desc->n_output);
				desc->output[desc->n_output++] = p;
			}
		} else if (FC_IS_PORT_CONTROL(fp->flags)) {
			if (FC_IS_PORT_INPUT(fp->flags)) {
				pw_log_info("using port %lu ('%s') as control %d", p,
						fp->name, desc->n_control);
				desc->control[desc->n_control++] = p;
			}
			else if (FC_IS_PORT_OUTPUT(fp->flags)) {
				pw_log_info("using port %lu ('%s') as notify %d", p,
						fp->name, desc->n_notify);
				desc->notify[desc->n_notify++] = p;
			}
		}
	}
	if (desc->n_input == 0 && desc->n_output == 0) {
		pw_log_error("plugin has no input and no output ports");
		res = -ENOTSUP;
		goto exit;
	}
	for (i = 0; i < desc->n_control; i++) {
		p = desc->control[i];
		desc->default_control[i] = get_default(impl, desc, p);
		pw_log_info("control %d ('%s') default to %f", i,
				d->ports[p].name, desc->default_control[i]);
	}
	spa_list_append(&hndl->descriptor_list, &desc->link);

	return desc;

exit:
	descriptor_unref(desc);
	errno = -res;
	return NULL;
}

/**
 * {
 *   ...
 * }
 */
static int parse_config(struct node *node, struct spa_json *config)
{
	const char *val;
	int len;

	if ((len = spa_json_next(config, &val)) <= 0)
		return len;

	if (spa_json_is_null(val, len))
		return 0;

	if (spa_json_is_container(val, len))
		len = spa_json_container_len(config, val, len);

	if ((node->config = malloc(len+1)) == NULL)
		return -errno;

	spa_json_parse_stringn(val, len, node->config, len+1);

	return 0;
}

/**
 * {
 *   "Reverb tail" = 2.0
 *   ...
 * }
 */
static int parse_control(struct node *node, struct spa_json *control)
{
	char key[256];

	while (spa_json_get_string(control, key, sizeof(key)) > 0) {
		float fl;
		const char *val;
		int res, len;

		if ((len = spa_json_next(control, &val)) < 0)
			break;

		if (spa_json_parse_float(val, len, &fl) <= 0) {
			pw_log_warn("control '%s' expects a number, ignoring", key);
		}
		else if ((res = set_control_value(node, key, &fl)) < 0) {
			pw_log_warn("control '%s' can not be set: %s", key, spa_strerror(res));
		}
	}
	return 0;
}

/**
 * output = [name:][portname]
 * input = [name:][portname]
 * ...
 */
static int parse_link(struct graph *graph, struct spa_json *json)
{
	char key[256];
	char output[256] = "";
	char input[256] = "";
	const char *val;
	struct node *def_in_node, *def_out_node;
	struct port *in_port, *out_port;
	struct link *link;

	if (spa_list_is_empty(&graph->node_list)) {
		pw_log_error("can't make links in graph without nodes");
		return -EINVAL;
	}

	while (spa_json_get_string(json, key, sizeof(key)) > 0) {
		if (spa_streq(key, "output")) {
			if (spa_json_get_string(json, output, sizeof(output)) <= 0) {
				pw_log_error("output expects a string");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "input")) {
			if (spa_json_get_string(json, input, sizeof(input)) <= 0) {
				pw_log_error("input expects a string");
				return -EINVAL;
			}
		}
		else {
			pw_log_error("unexpected link key '%s'", key);
			if (spa_json_next(json, &val) < 0)
				break;
		}
	}
	def_out_node = spa_list_first(&graph->node_list, struct node, link);
	def_in_node = spa_list_last(&graph->node_list, struct node, link);

	out_port = find_port(def_out_node, output, FC_PORT_OUTPUT);
	in_port = find_port(def_in_node, input, FC_PORT_INPUT);

	if (out_port == NULL && out_port == NULL) {
		/* try control ports */
		out_port = find_port(def_out_node, output, FC_PORT_OUTPUT | FC_PORT_CONTROL);
		in_port = find_port(def_in_node, input, FC_PORT_INPUT | FC_PORT_CONTROL);
	}
	if (in_port == NULL || out_port == NULL) {
		if (out_port == NULL)
			pw_log_error("unknown output port %s", output);
		if (in_port == NULL)
			pw_log_error("unknown input port %s", input);
		return -ENOENT;
	}

	if (in_port->n_links > 0) {
		pw_log_info("Can't have more than 1 link to %s, use a mixer", input);
		return -ENOTSUP;
	}

	if ((link = calloc(1, sizeof(*link))) == NULL)
		return -errno;

	link->output = out_port;
	link->input = in_port;

	pw_log_info("linking %s:%s -> %s:%s",
			out_port->node->name,
			out_port->node->desc->desc->ports[out_port->p].name,
			in_port->node->name,
			in_port->node->desc->desc->ports[in_port->p].name);

	spa_list_append(&out_port->link_list, &link->output_link);
	out_port->n_links++;
	spa_list_append(&in_port->link_list, &link->input_link);
	in_port->n_links++;

	in_port->node->n_deps++;

	spa_list_append(&graph->link_list, &link->link);

	return 0;
}

static void link_free(struct link *link)
{
	spa_list_remove(&link->input_link);
	link->input->n_links--;
	link->input->node->n_deps--;
	spa_list_remove(&link->output_link);
	link->output->n_links--;
	spa_list_remove(&link->link);
	free(link);
}

/**
 * {
 *   control = [name:][portname]
 *   min = <float, default 0.0>
 *   max = <float, default 1.0>
 *   scale = <string, default "linear", options "linear","cubic">
 * }
 */
static int parse_volume(struct graph *graph, struct spa_json *json, bool capture)
{
	char key[256];
	char control[256] = "";
	char scale[64] = "linear";
	float min = 0.0f, max = 1.0f;
	const char *val;
	struct node *def_control;
	struct port *port;
	struct volume *vol = capture ? &graph->capture_volume :
		&graph->playback_volume;

	if (spa_list_is_empty(&graph->node_list)) {
		pw_log_error("can't set volume in graph without nodes");
		return -EINVAL;
	}
	while (spa_json_get_string(json, key, sizeof(key)) > 0) {
		if (spa_streq(key, "control")) {
			if (spa_json_get_string(json, control, sizeof(control)) <= 0) {
				pw_log_error("control expects a string");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "min")) {
			if (spa_json_get_float(json, &min) <= 0) {
				pw_log_error("min expects a float");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "max")) {
			if (spa_json_get_float(json, &max) <= 0) {
				pw_log_error("max expects a float");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "scale")) {
			if (spa_json_get_string(json, scale, sizeof(scale)) <= 0) {
				pw_log_error("scale expects a string");
				return -EINVAL;
			}
		}
		else {
			pw_log_error("unexpected volume key '%s'", key);
			if (spa_json_next(json, &val) < 0)
				break;
		}
	}
	if (capture)
		def_control = spa_list_first(&graph->node_list, struct node, link);
	else
		def_control = spa_list_last(&graph->node_list, struct node, link);

	port = find_port(def_control, control, FC_PORT_INPUT | FC_PORT_CONTROL);
	if (port == NULL) {
		pw_log_error("unknown control port %s", control);
		return -ENOENT;
	}
	if (vol->n_ports >= SPA_AUDIO_MAX_CHANNELS) {
		pw_log_error("too many volume controls");
		return -ENOSPC;
	}
	if (spa_streq(scale, "linear")) {
		vol->scale[vol->n_ports] = SCALE_LINEAR;
	} else if (spa_streq(scale, "cubic")) {
		vol->scale[vol->n_ports] = SCALE_CUBIC;
	} else {
		pw_log_error("Invalid scale value '%s', use one of linear or cubic", scale);
		return -EINVAL;
	}
	pw_log_info("volume %d: \"%s:%s\" min:%f max:%f scale:%s", vol->n_ports, port->node->name,
			port->node->desc->desc->ports[port->p].name, min, max, scale);

	vol->ports[vol->n_ports] = port;
	vol->min[vol->n_ports] = min;
	vol->max[vol->n_ports] = max;
	vol->n_ports++;

	return 0;
}

/**
 * type = ladspa
 * name = rev
 * plugin = g2reverb
 * label = G2reverb
 * config = {
 *     ...
 * }
 * control = {
 *     ...
 * }
 */
static int load_node(struct graph *graph, struct spa_json *json)
{
	struct spa_json control, config;
	struct descriptor *desc;
	struct node *node;
	const char *val;
	char key[256];
	char type[256] = "";
	char name[256] = "";
	char plugin[256] = "";
	char label[256] = "";
	bool have_control = false;
	bool have_config = false;
	uint32_t i;
	int res;

	while (spa_json_get_string(json, key, sizeof(key)) > 0) {
		if (spa_streq("type", key)) {
			if (spa_json_get_string(json, type, sizeof(type)) <= 0) {
				pw_log_error("type expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("name", key)) {
			if (spa_json_get_string(json, name, sizeof(name)) <= 0) {
				pw_log_error("name expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("plugin", key)) {
			if (spa_json_get_string(json, plugin, sizeof(plugin)) <= 0) {
				pw_log_error("plugin expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("label", key)) {
			if (spa_json_get_string(json, label, sizeof(label)) <= 0) {
				pw_log_error("label expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("control", key)) {
			if (spa_json_enter_object(json, &control) <= 0) {
				pw_log_error("control expects an object");
				return -EINVAL;
			}
			have_control = true;
		} else if (spa_streq("config", key)) {
			config = SPA_JSON_SAVE(json);
			have_config = true;
			if (spa_json_next(json, &val) < 0)
				break;
		} else {
			pw_log_warn("unexpected node key '%s'", key);
			if (spa_json_next(json, &val) < 0)
				break;
		}
	}
	if (spa_streq(type, "builtin"))
		snprintf(plugin, sizeof(plugin), "%s", "builtin");
	else if (spa_streq(type, "")) {
		pw_log_error("missing plugin type");
		return -EINVAL;
	}

	pw_log_info("loading type:%s plugin:%s label:%s", type, plugin, label);

	if ((desc = descriptor_load(graph->impl, type, plugin, label)) == NULL)
		return -errno;

	node = calloc(1, sizeof(*node));
	if (node == NULL)
		return -errno;

	node->graph = graph;
	node->desc = desc;
	snprintf(node->name, sizeof(node->name), "%s", name);

	node->input_port = calloc(desc->n_input, sizeof(struct port));
	node->output_port = calloc(desc->n_output, sizeof(struct port));
	node->control_port = calloc(desc->n_control, sizeof(struct port));
	node->notify_port = calloc(desc->n_notify, sizeof(struct port));

	pw_log_info("loaded n_input:%d n_output:%d n_control:%d n_notify:%d",
			desc->n_input, desc->n_output,
			desc->n_control, desc->n_notify);

	for (i = 0; i < desc->n_input; i++) {
		struct port *port = &node->input_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->input[i];
		spa_list_init(&port->link_list);
	}
	for (i = 0; i < desc->n_output; i++) {
		struct port *port = &node->output_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->output[i];
		spa_list_init(&port->link_list);
	}
	for (i = 0; i < desc->n_control; i++) {
		struct port *port = &node->control_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->control[i];
		spa_list_init(&port->link_list);
		port->control_data[0] = desc->default_control[i];
	}
	for (i = 0; i < desc->n_notify; i++) {
		struct port *port = &node->notify_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->notify[i];
		spa_list_init(&port->link_list);
	}
	if (have_config)
		if ((res = parse_config(node, &config)) < 0)
			pw_log_warn("error parsing config: %s", spa_strerror(res));
	if (have_control)
		parse_control(node, &control);

	spa_list_append(&graph->node_list, &node->link);

	return 0;
}

static void node_cleanup(struct node *node)
{
	const struct fc_descriptor *d = node->desc->desc;
	uint32_t i;

	for (i = 0; i < node->n_hndl; i++) {
		if (node->hndl[i] == NULL)
			continue;
		pw_log_info("cleanup %s %d", d->name, i);
		if (d->deactivate)
			d->deactivate(node->hndl[i]);
		d->cleanup(node->hndl[i]);
		node->hndl[i] = NULL;
	}
}

static int port_ensure_data(struct port *port, uint32_t i, uint32_t max_samples)
{
	float *data;
	if ((data = port->audio_data[i]) == NULL) {
		data = calloc(max_samples, sizeof(float));
		if (data == NULL) {
			pw_log_error("cannot create port data: %m");
			return -errno;
		}
	}
	port->audio_data[i] = data;
	return 0;
}

static void port_free_data(struct port *port, uint32_t i)
{
	free(port->audio_data[i]);
	port->audio_data[i] = NULL;
}

static void node_free(struct node *node)
{
	uint32_t i, j;

	spa_list_remove(&node->link);
	for (i = 0; i < node->n_hndl; i++) {
		for (j = 0; j < node->desc->n_output; j++)
			port_free_data(&node->output_port[j], i);
	}
	node_cleanup(node);
	descriptor_unref(node->desc);
	free(node->input_port);
	free(node->output_port);
	free(node->control_port);
	free(node->notify_port);
	free(node->config);
	free(node);
}

static void graph_cleanup(struct graph *graph)
{
	struct node *node;
	if (!graph->instantiated)
		return;
	graph->instantiated = false;
	spa_list_for_each(node, &graph->node_list, link)
		node_cleanup(node);
}

static int graph_instantiate(struct graph *graph)
{
	struct impl *impl = graph->impl;
	struct node *node;
	struct port *port;
	struct link *link;
	struct descriptor *desc;
	const struct fc_descriptor *d;
	uint32_t i, j, max_samples = impl->quantum_limit;
	int res;
	float *sd = impl->silence_data, *dd = impl->discard_data;

	if (graph->instantiated)
		return 0;

	graph->instantiated = true;

	spa_list_for_each(node, &graph->node_list, link) {

		node_cleanup(node);

		desc = node->desc;
		d = desc->desc;
		if (d->flags & FC_DESCRIPTOR_SUPPORTS_NULL_DATA)
			sd = dd = NULL;

		for (i = 0; i < node->n_hndl; i++) {
			pw_log_info("instantiate %s %d rate:%lu", d->name, i, impl->rate);
			errno = EINVAL;
			if ((node->hndl[i] = d->instantiate(d, impl->rate, i, node->config)) == NULL) {
				pw_log_error("cannot create plugin instance %d rate:%lu: %m", i, impl->rate);
				res = -errno;
				goto error;
			}
			for (j = 0; j < desc->n_input; j++) {
				port = &node->input_port[j];
				d->connect_port(node->hndl[i], port->p, sd);

				spa_list_for_each(link, &port->link_list, input_link) {
					struct port *peer = link->output;
					if ((res = port_ensure_data(peer, i, max_samples)) < 0)
						goto error;
					pw_log_info("connect input port %s[%d]:%s %p",
							node->name, i, d->ports[port->p].name,
							peer->audio_data[i]);
					d->connect_port(node->hndl[i], port->p, peer->audio_data[i]);
				}
			}
			for (j = 0; j < desc->n_output; j++) {
				port = &node->output_port[j];
				if ((res = port_ensure_data(port, i, max_samples)) < 0)
					goto error;
				pw_log_info("connect output port %s[%d]:%s %p",
						node->name, i, d->ports[port->p].name,
						port->audio_data[i]);
				d->connect_port(node->hndl[i], port->p, port->audio_data[i]);
			}
			for (j = 0; j < desc->n_control; j++) {
				port = &node->control_port[j];
				d->connect_port(node->hndl[i], port->p, &port->control_data[i]);

				spa_list_for_each(link, &port->link_list, input_link) {
					struct port *peer = link->output;
					pw_log_info("connect control port %s[%d]:%s %p",
							node->name, i, d->ports[port->p].name,
							&peer->control_data[i]);
					d->connect_port(node->hndl[i], port->p, &peer->control_data[i]);
				}
			}
			for (j = 0; j < desc->n_notify; j++) {
				port = &node->notify_port[j];
				pw_log_info("connect notify port %s[%d]:%s %p",
						node->name, i, d->ports[port->p].name,
						&port->control_data[i]);
				d->connect_port(node->hndl[i], port->p, &port->control_data[i]);
			}
			if (d->activate)
				d->activate(node->hndl[i]);
			if (node->control_changed && d->control_changed)
				d->control_changed(node->hndl[i]);
		}
	}
	update_props_param(impl);
	return 0;
error:
	graph_cleanup(graph);
	return res;
}

/* any default values for the controls are set in the first instance
 * of the control data. Duplicate this to the other instances now. */
static void setup_node_controls(struct node *node)
{
	uint32_t i, j;
	uint32_t n_hndl = node->n_hndl;
	uint32_t n_ports = node->desc->n_control;
	struct port *ports = node->control_port;

	for (i = 0; i < n_ports; i++) {
		struct port *port = &ports[i];
		for (j = 1; j < n_hndl; j++)
			port->control_data[j] = port->control_data[0];
	}
}

static struct node *find_next_node(struct graph *graph)
{
	struct node *node;
	spa_list_for_each(node, &graph->node_list, link) {
		if (node->n_deps == 0 && !node->visited) {
			node->visited = true;
			return node;
		}
	}
	return NULL;
}

static int setup_graph(struct graph *graph, struct spa_json *inputs, struct spa_json *outputs)
{
	struct impl *impl = graph->impl;
	struct node *node, *first, *last;
	struct port *port;
	struct link *link;
	struct graph_port *gp;
	struct graph_hndl *gh;
	uint32_t i, j, n_nodes, n_input, n_output, n_control, n_hndl = 0;
	int res;
	struct descriptor *desc;
	const struct fc_descriptor *d;
	char v[256];

	first = spa_list_first(&graph->node_list, struct node, link);
	last = spa_list_last(&graph->node_list, struct node, link);

	/* calculate the number of inputs and outputs into the graph.
	 * If we have a list of inputs/outputs, just count them. Otherwise
	 * we count all input ports of the first node and all output
	 * ports of the last node */
	if (inputs != NULL) {
		n_input = count_array(inputs);
	} else {
		n_input = first->desc->n_input;
	}
	if (outputs != NULL) {
		n_output = count_array(outputs);
	} else {
		n_output = last->desc->n_output;
	}
	if (n_input == 0) {
		pw_log_error("no inputs");
		res = -EINVAL;
		goto error;
	}
	if (n_output == 0) {
		pw_log_error("no outputs");
		res = -EINVAL;
		goto error;
	}

	if (impl->capture_info.channels == 0)
		impl->capture_info.channels = n_input;
	if (impl->playback_info.channels == 0)
		impl->playback_info.channels = n_output;

	/* compare to the requested number of channels and duplicate the
	 * graph n_hndl times when needed. */
	n_hndl = impl->capture_info.channels / n_input;
	if (n_hndl != impl->playback_info.channels / n_output) {
		pw_log_error("invalid channels. The capture stream has %1$d channels and "
				"the filter has %2$d inputs. The playback stream has %3$d channels "
				"and the filter has %4$d outputs. capture:%1$d / input:%2$d != "
				"playback:%3$d / output:%4$d. Check inputs and outputs objects.",
				impl->capture_info.channels, n_input,
				impl->playback_info.channels, n_output);
		res = -EINVAL;
		goto error;
	}
	if (n_hndl > MAX_HNDL) {
		pw_log_error("too many channels. %d > %d", n_hndl, MAX_HNDL);
		res = -EINVAL;
		goto error;
	}
	if (n_hndl == 0) {
		n_hndl = 1;
		pw_log_warn("The capture stream has %1$d channels and "
				"the filter has %2$d inputs. The playback stream has %3$d channels "
				"and the filter has %4$d outputs. Some filter ports will be "
				"unconnected..",
				impl->capture_info.channels, n_input,
				impl->playback_info.channels, n_output);
	}
	pw_log_info("using %d instances %d %d", n_hndl, n_input, n_output);

	/* now go over all nodes and create instances. */
	n_control = 0;
	n_nodes = 0;
	spa_list_for_each(node, &graph->node_list, link) {
		node->n_hndl = n_hndl;
		desc = node->desc;
		n_control += desc->n_control;
		n_nodes++;
		setup_node_controls(node);
	}
	graph->n_input = 0;
	graph->input = calloc(n_input * 16 * n_hndl, sizeof(struct graph_port));
	graph->n_output = 0;
	graph->output = calloc(n_output * n_hndl, sizeof(struct graph_port));

	/* now collect all input and output ports for all the handles. */
	for (i = 0; i < n_hndl; i++) {
		if (inputs == NULL) {
			desc = first->desc;
			d = desc->desc;
			for (j = 0; j < desc->n_input; j++) {
				gp = &graph->input[graph->n_input++];
				pw_log_info("input port %s[%d]:%s",
						first->name, i, d->ports[desc->input[j]].name);
				gp->desc = d;
				gp->hndl = &first->hndl[i];
				gp->port = desc->input[j];
			}
		} else {
			struct spa_json it = *inputs;
			while (spa_json_get_string(&it, v, sizeof(v)) > 0) {
				if (spa_streq(v, "null")) {
					gp = &graph->input[graph->n_input++];
					gp->desc = NULL;
					pw_log_info("ignore input port %d", graph->n_input);
				} else if ((port = find_port(first, v, FC_PORT_INPUT)) == NULL) {
					res = -ENOENT;
					pw_log_error("input port %s not found", v);
					goto error;
				} else {
					bool disabled = false;

					desc = port->node->desc;
					d = desc->desc;
					if (i == 0 && port->external != SPA_ID_INVALID) {
						pw_log_error("input port %s[%d]:%s already used as input %d, use mixer",
							port->node->name, i, d->ports[port->p].name,
							port->external);
						res = -EBUSY;
						goto error;
					}
					if (port->n_links > 0) {
						pw_log_error("input port %s[%d]:%s already used by link, use mixer",
							port->node->name, i, d->ports[port->p].name);
						res = -EBUSY;
						goto error;
					}

					if (d->flags & FC_DESCRIPTOR_COPY) {
						for (j = 0; j < desc->n_output; j++) {
							struct port *p = &port->node->output_port[j];
							struct link *link;

							gp = NULL;
							spa_list_for_each(link, &p->link_list, output_link) {
								struct port *peer = link->input;

								pw_log_info("copy input port %s[%d]:%s",
									port->node->name, i,
									d->ports[port->p].name);
								peer->external = graph->n_input;
								gp = &graph->input[graph->n_input++];
								gp->desc = peer->node->desc->desc;
								gp->hndl = &peer->node->hndl[i];
								gp->port = peer->p;
								gp->next = true;
								disabled = true;
							}
							if (gp != NULL)
								gp->next = false;
						}
						port->node->disabled = disabled;
					}
					if (!disabled) {
						pw_log_info("input port %s[%d]:%s",
							port->node->name, i, d->ports[port->p].name);
						port->external = graph->n_input;
						gp = &graph->input[graph->n_input++];
						gp->desc = d;
						gp->hndl = &port->node->hndl[i];
						gp->port = port->p;
						gp->next = false;
					}
				}
			}
		}
		if (outputs == NULL) {
			desc = last->desc;
			d = desc->desc;
			for (j = 0; j < desc->n_output; j++) {
				gp = &graph->output[graph->n_output++];
				pw_log_info("output port %s[%d]:%s",
						last->name, i, d->ports[desc->output[j]].name);
				gp->desc = d;
				gp->hndl = &last->hndl[i];
				gp->port = desc->output[j];
			}
		} else {
			struct spa_json it = *outputs;
			while (spa_json_get_string(&it, v, sizeof(v)) > 0) {
				gp = &graph->output[graph->n_output];
				if (spa_streq(v, "null")) {
					gp->desc = NULL;
					pw_log_info("silence output port %d", graph->n_output);
				} else if ((port = find_port(last, v, FC_PORT_OUTPUT)) == NULL) {
					res = -ENOENT;
					pw_log_error("output port %s not found", v);
					goto error;
				} else {
					desc = port->node->desc;
					d = desc->desc;
					if (i == 0 && port->external != SPA_ID_INVALID) {
						pw_log_error("output port %s[%d]:%s already used as output %d, use copy",
							port->node->name, i, d->ports[port->p].name,
							port->external);
						res = -EBUSY;
						goto error;
					}
					if (port->n_links > 0) {
						pw_log_error("output port %s[%d]:%s already used by link, use copy",
							port->node->name, i, d->ports[port->p].name);
						res = -EBUSY;
						goto error;
					}
					pw_log_info("output port %s[%d]:%s",
							port->node->name, i, d->ports[port->p].name);
					port->external = graph->n_output;
					gp->desc = d;
					gp->hndl = &port->node->hndl[i];
					gp->port = port->p;
				}
				graph->n_output++;
			}
		}
	}

	/* order all nodes based on dependencies */
	graph->n_hndl = 0;
	graph->hndl = calloc(n_nodes * n_hndl, sizeof(struct graph_hndl));
	graph->n_control = 0;
	graph->control_port = calloc(n_control, sizeof(struct port *));
	while (true) {
		if ((node = find_next_node(graph)) == NULL)
			break;

		desc = node->desc;
		d = desc->desc;

		if (!node->disabled) {
			for (i = 0; i < n_hndl; i++) {
				gh = &graph->hndl[graph->n_hndl++];
				gh->hndl = &node->hndl[i];
				gh->desc = d;
			}
		}
		for (i = 0; i < desc->n_output; i++) {
			spa_list_for_each(link, &node->output_port[i].link_list, output_link)
				link->input->node->n_deps--;
		}
		for (i = 0; i < desc->n_notify; i++) {
			spa_list_for_each(link, &node->notify_port[i].link_list, output_link)
				link->input->node->n_deps--;
		}

		/* collect all control ports on the graph */
		for (i = 0; i < desc->n_control; i++) {
			graph->control_port[graph->n_control] = &node->control_port[i];
			graph->n_control++;
		}
	}
	res = 0;
error:
	return res;
}

/**
 * filter.graph = {
 *     nodes = [
 *         { ... } ...
 *     ]
 *     links = [
 *         { ... } ...
 *     ]
 *     inputs = [ ]
 *     outputs = [ ]
 * }
 */
static int load_graph(struct graph *graph, struct pw_properties *props)
{
	struct spa_json it[3];
	struct spa_json inputs, outputs, *pinputs = NULL, *poutputs = NULL;
	struct spa_json cvolumes, pvolumes, *pcvolumes = NULL, *ppvolumes = NULL;
	struct spa_json nodes, *pnodes = NULL, links, *plinks = NULL;
	const char *json, *val;
	char key[256];
	int res;

	spa_list_init(&graph->node_list);
	spa_list_init(&graph->link_list);

	if ((json = pw_properties_get(props, "filter.graph")) == NULL) {
		pw_log_error("missing filter.graph property");
		return -EINVAL;
	}

	spa_json_init(&it[0], json, strlen(json));
        if (spa_json_enter_object(&it[0], &it[1]) <= 0) {
		pw_log_error("filter.graph must be an object");
		return -EINVAL;
	}

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_streq("nodes", key)) {
			if (spa_json_enter_array(&it[1], &nodes) <= 0) {
				pw_log_error("nodes expects an array");
				return -EINVAL;
			}
			pnodes = &nodes;
		}
		else if (spa_streq("links", key)) {
			if (spa_json_enter_array(&it[1], &links) <= 0) {
				pw_log_error("links expects an array");
				return -EINVAL;
			}
			plinks = &links;
		}
		else if (spa_streq("inputs", key)) {
			if (spa_json_enter_array(&it[1], &inputs) <= 0) {
				pw_log_error("inputs expects an array");
				return -EINVAL;
			}
			pinputs = &inputs;
		}
		else if (spa_streq("outputs", key)) {
			if (spa_json_enter_array(&it[1], &outputs) <= 0) {
				pw_log_error("outputs expects an array");
				return -EINVAL;
			}
			poutputs = &outputs;
		}
		else if (spa_streq("capture.volumes", key)) {
			if (spa_json_enter_array(&it[1], &cvolumes) <= 0) {
				pw_log_error("capture.volumes expects an array");
				return -EINVAL;
			}
			pcvolumes = &cvolumes;
		}
		else if (spa_streq("playback.volumes", key)) {
			if (spa_json_enter_array(&it[1], &pvolumes) <= 0) {
				pw_log_error("playback.volumes expects an array");
				return -EINVAL;
			}
			ppvolumes = &pvolumes;
		} else {
			pw_log_warn("unexpected graph key '%s'", key);
			if (spa_json_next(&it[1], &val) < 0)
				break;
		}
	}
	if (pnodes == NULL) {
		pw_log_error("filter.graph is missing a nodes array");
		return -EINVAL;
	}
	while (spa_json_enter_object(pnodes, &it[2]) > 0) {
		if ((res = load_node(graph, &it[2])) < 0)
			return res;
	}
	if (plinks != NULL) {
		while (spa_json_enter_object(plinks, &it[2]) > 0) {
			if ((res = parse_link(graph, &it[2])) < 0)
				return res;
		}
	}
	if (pcvolumes != NULL) {
		while (spa_json_enter_object(pcvolumes, &it[2]) > 0) {
			if ((res = parse_volume(graph, &it[2], true)) < 0)
				return res;
		}
	}
	if (ppvolumes != NULL) {
		while (spa_json_enter_object(ppvolumes, &it[2]) > 0) {
			if ((res = parse_volume(graph, &it[2], false)) < 0)
				return res;
		}
	}
	return setup_graph(graph, pinputs, poutputs);
}

static void graph_free(struct graph *graph)
{
	struct link *link;
	struct node *node;
	spa_list_consume(link, &graph->link_list, link)
		link_free(link);
	spa_list_consume(node, &graph->node_list, link)
		node_free(node);
	free(graph->input);
	free(graph->output);
	free(graph->hndl);
	free(graph->control_port);
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	if (res == -ENOENT) {
		pw_log_info("message id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
	} else {
		pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
	}

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
	struct plugin_func *pl;

	/* disconnect both streams before destroying any of them */
	if (impl->capture)
		pw_stream_disconnect(impl->capture);
	if (impl->playback)
		pw_stream_disconnect(impl->playback);

	if (impl->capture)
		pw_stream_destroy(impl->capture);
	if (impl->playback)
		pw_stream_destroy(impl->playback);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_properties_free(impl->capture_props);
	pw_properties_free(impl->playback_props);
	graph_free(&impl->graph);
	spa_list_consume(pl, &impl->plugin_func_list, link)
		free_plugin_func(pl);

	free(impl->silence_data);
	free(impl->discard_data);
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

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	*info = SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_F32P);
	info->rate = pw_properties_get_int32(props, PW_KEY_AUDIO_RATE, info->rate);
	info->channels = pw_properties_get_int32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->capture_props, key) == NULL)
			pw_properties_set(impl->capture_props, key, str);
		if (pw_properties_get(impl->playback_props, key) == NULL)
			pw_properties_set(impl->playback_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t pid = getpid();
	const char *str;
	int res;
	const struct spa_support *support;
	uint32_t n_support;
	struct spa_cpu *cpu_iface;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = pw_properties_new(NULL, NULL);

	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->graph.impl = impl;

	spa_list_init(&impl->plugin_list);
	spa_list_init(&impl->plugin_func_list);

	add_plugin_func(impl, "builtin", load_builtin_plugin, NULL);
	add_plugin_func(impl, "ladspa", load_ladspa_plugin, NULL);

	support = pw_context_get_support(impl->context, &n_support);
	impl->quantum_limit = pw_properties_get_uint32(
			pw_context_get_properties(impl->context),
			"default.clock.quantum-limit", 8192u);

	impl->silence_data = calloc(impl->quantum_limit, sizeof(float));
	if (impl->silence_data == NULL) {
		res = -errno;
		goto error;
	}

	impl->discard_data = calloc(impl->quantum_limit, sizeof(float));
	if (impl->discard_data == NULL) {
		res = -errno;
		goto error;
	}

	cpu_iface = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	impl->dsp.cpu_flags = cpu_iface ? spa_cpu_get_flags(cpu_iface) : 0;
	dsp_ops_init(&impl->dsp);

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "filter-chain-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_LINK_GROUP, "filter-chain-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, "resample.prefill") == NULL)
		pw_properties_set(props, "resample.prefill", "true");
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "filter-chain-%u-%u", pid, id);

	if ((str = pw_properties_get(props, "capture.props")) != NULL)
		pw_properties_update_string(impl->capture_props, str, strlen(str));
	if ((str = pw_properties_get(props, "playback.props")) != NULL)
		pw_properties_update_string(impl->playback_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LINK_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_MEDIA_NAME);
	copy_props(impl, props, "resample.prefill");

	parse_audio_info(impl->capture_props, &impl->capture_info);
	parse_audio_info(impl->playback_props, &impl->playback_info);

	if (!impl->capture_info.rate && !impl->playback_info.rate) {
		if (pw_properties_get(impl->playback_props, "resample.disable") == NULL)
			pw_properties_set(impl->playback_props, "resample.disable", "true");
		if (pw_properties_get(impl->capture_props, "resample.disable") == NULL)
			pw_properties_set(impl->capture_props, "resample.disable", "true");
	} else if (impl->capture_info.rate && !impl->playback_info.rate)
		impl->playback_info.rate = impl->capture_info.rate;
	else if (impl->playback_info.rate && !impl->capture_info.rate)
		impl->capture_info.rate = !impl->playback_info.rate;
	else if (impl->capture_info.rate != impl->playback_info.rate) {
		pw_log_warn("Both capture and playback rate are set, but"
			" they are different. Using the highest of two. This behaviour"
			" is deprecated, please use equal rates in the module config");
		impl->playback_info.rate = impl->capture_info.rate =
			SPA_MAX(impl->playback_info.rate, impl->capture_info.rate);
	}

	if ((str = pw_properties_get(props, PW_KEY_NODE_NAME)) == NULL) {
		pw_properties_setf(props, PW_KEY_NODE_NAME,
				"filter-chain-%u-%u", pid, id);
		str = pw_properties_get(props, PW_KEY_NODE_NAME);
	}
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_NAME,
				"input.%s", str);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_NAME,
				"output.%s", str);

	if (pw_properties_get(impl->capture_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_MEDIA_NAME, "%s input",
				pw_properties_get(impl->capture_props, PW_KEY_NODE_DESCRIPTION));
	if (pw_properties_get(impl->playback_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_MEDIA_NAME, "%s output",
				pw_properties_get(impl->playback_props, PW_KEY_NODE_DESCRIPTION));

	if ((res = load_graph(&impl->graph, props)) < 0) {
		pw_log_error("can't load graph: %s", spa_strerror(res));
		goto error;
	}

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
	pw_properties_free(props);

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	setup_streams(impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	pw_properties_free(props);
	impl_destroy(impl);
	return res;
}
