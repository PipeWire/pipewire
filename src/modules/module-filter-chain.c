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

#include <spa/param/latency-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/pod/dynamic.h>
#include <spa/filter-graph/filter-graph.h>

#include <pipewire/impl.h>

#define NAME "filter-chain"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

extern struct spa_handle_factory spa_filter_graph_factory;

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
 * - `type` is one of `ladspa`, `lv2`, `builtin`, `sofa` or `ebur128`.
 * - `name` is the name for this node, you might need this later to refer to this node
 *    and its ports when setting controls or making links.
 * - `plugin` is the type specific plugin name.
 *    - For LADSPA plugins it will append `.so` to find the shared object with that
 *       name in the LADSPA plugin path.
 *    - For LV2, this is the plugin URI obtained with lv2ls.
 *    - For builtin, sofa and ebur128 this is ignored
 * - `label` is the type specific filter inside the plugin.
 *    - For LADSPA this is the label
 *    - For LV2 this is unused
 *    - For builtin, sofa and ebur128 this is the name of the filter to use
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
 * links can be omitted when the graph has just 1 filter.
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
 * ### Parametric EQ
 *
 * The parametric EQ chains a number of biquads together. It is more efficient than
 * specifying a number of chained biquads and it can also load configuration from a
 * file.
 *
 * The parametric EQ supports multichannel processing and has 8 input and 8 output ports
 * that don't all need to be connected. The ports are named `In 1` to `In 8` and
 * `Out 1` to `Out 8`.
 *
 *\code{.unparsed}
 * filter.graph = {
 *     nodes = [
 *         {
 *             type   = builtin
 *             name   = ...
 *             label  = param_eq
 *             config = {
 *                 filename = "..."
 *                 #filename1 = "...", filename2 = "...", ...
 *                 filters = [
 *                     { type = ..., freq = ..., gain = ..., q = ... },
 *                     { type = ..., freq = ..., gain = ..., q = ... },
 *                     ....
 *                 ]
 *                 #filters1 = [ ... ], filters2 = [ ... ], ...
 *             }
 *             ...
 *         }
 *     }
 *     ...
 * }
 *\endcode
 *
 * Either a `filename` or a `filters` array can be specified. The configuration
 * will be used for all channels. Alternatively `filenameX` or `filtersX` where
 * X is the channel number (between 1 and 8) can be used to load a channel
 * specific configuration.
 *
 * The `filename` must point to a parametric equalizer configuration
 * generated from the AutoEQ project or Squiglink. Both the projects allow
 * equalizing headphones or an in-ear monitor to a target curve.
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
 * The `filters` (or channel specific `filtersX` where X is the channel between 1 and
 * 8) can contain an array of filter specification object with the following keys:
 *
 *   `type` specifies the filter type, choose one from the available biquad labels.
 *   `freq` is the frequency passed to the biquad.
 *   `gain` is the gain passed to the biquad.
 *   `q` is the Q passed to the biquad.
 *
 * This makes it possible to also use the param eq without a file and with all the
 * available biquads.
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
 * - `delay`    The extra delay to add to the IR. A float number will be interpreted as seconds,
 *              and integer as samples. Using the delay in seconds is independent of the graph
 *              and IR rate and is recommended.
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
 * ### Abs
 *
 * The abs plugin can be used to calculate the absolute value of samples.
 *
 * It has an input port "In" and an output port "Out".
 *
 * ### Sqrt
 *
 * The sqrt plugin can be used to calculate the square root of samples.
 *
 * It has an input port "In" and an output port "Out".
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
 * frequency, amplitude, offset and phase.
 *
 * ### Max
 *
 * Use the `max` plugin if you need to select the max value of two channels.
 *
 * It has two input ports "In 1" and "In 2" and one output port "Out".
 *
 * ### dcblock
 *
 * Use the `dcblock` plugin implements a [DC blocker](https://www.dsprelated.com/freebooks/filters/DC_Blocker.html).
 *
 * It has 8 input ports "In 1" to "In 8" and corresponding output ports "Out 1"
 * to "Out 8". Not all ports need to be connected.
 *
 * It also has 1 control input port "R" that controls the DC block R factor.
 *
 * ### Ramp
 *
 * Use the `ramp` plugin creates a linear ramp from `Start` to `Stop`.
 *
 * It has 3 input control ports "Start", "Stop" and "Duration (s)". It also has one
 * output port "Out". A linear ramp will be created from "Start" to "Stop" for a duration
 * given by the "Duration (s)" control in (fractional) seconds. The current value will
 * be stored in the output notify port "Current".
 *
 * The ramp output can, for example, be used as input for the `mult` plugin to create
 * a volume ramp up or down. For more a more coarse volume ramp, the "Current" value
 * can be used in the `linear` plugin.
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
 * ## EBUR128 filter
 *
 * There is an optional EBU R128 filter available.
 *
 * ### ebur128
 *
 * The ebur128 plugin can be used to measure the loudness of a signal.
 *
 * It has 7 input ports "In FL", "In FR", "In FC", "In UNUSED", "In SL", "In SR"
 * and "In DUAL MONO", corresponding to the different input channels for EBUR128.
 * Not all ports need to be connected for this filter.
 *
 * The input signal is passed unmodified on the "Out FL", "Out FR", "Out FC",
 * "Out UNUSED", "Out SL", "Out SR" and "Out DUAL MONO" output ports.
 *
 * There are 7 output control ports that contain the measured loudness information
 * and that can be used to control the processing of the audio. Some of these ports
 * contain values in LUFS, or "Loudness Units relative to Full Scale". These are
 * negative values, closer to 0 is louder. You can use the lufs2gain plugin to
 * convert this value to again to adjust a volume (See below).
 *
 * "Momentary LUFS" contains the momentary loudness measurement with a 400ms window
 *                  and 75% overlap. It works mostly like an R.M.S. meter.
 *
 * "Shortterm LUFS" contains the shortterm loudness in LUFS over a 3 second window.
 *
 * "Global LUFS" contains the global integrated loudness in LUFS over the max-history
 *               window.
 * "Window LUFS" contains the global integrated loudness in LUFS over the max-window
 *               window.
 *
 * "Range LU" contains the loudness range (LRA) in LU units.
 *
 * "Peak" contains the peak loudness.
 *
 * "True Peak" contains the true peak loudness oversampling the signal. This can more
 *             accurately reflect the peak compared to "Peak".
 *
 * The node also has an optional `config` section with extra configuration:
 *
 *\code{.unparsed}
 * filter.graph = {
 *     nodes = [
 *         {
 *             type   = ebur128
 *             name   = ...
 *             label  = ebur128
 *             config = {
 *                 max-history = ...
 *                 max-window = ...
 *                 use-histogram = ...
 *             }
 *             ...
 *         }
 *     }
 *     ...
 * }
 *\endcode
 *
 * - `max-history` the maximum history to keep in (float) seconds. Default to 10.0
 *
 * - `max-window` the maximum window to keep in (float) seconds. Default to 0.0
 *                You will need to set this to some value to get "Window LUFS"
 *                output control values.
 *
 * - `use-histogram` uses the histogram algorithm to calculate loudness. Defaults
 *                   to false.
 *
 * ### lufs2gain
 *
 * The lufs2gain plugin can be used to convert LUFS control values to gain. It needs
 * a target LUFS control input to drive the conversion.
 *
 * It has 2 input control ports "LUFS" and "Target LUFS" and will produce 1 output
 * control value "Gain". This gain can be used as input for the builtin `linear`
 * node, for example, to adust the gain.
 *
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
 * - \ref PW_KEY_NODE_NAME : See notes below. If not specified, defaults to
 *	'filter-chain-PID-MODULEID'.
 *
 * Stream only properties:
 *
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_NODE_NAME :  if not given per stream, the global node.name will be
 *         prefixed with 'input.' and 'output.' to generate a capture and playback
 *         stream node.name respectively.
 *
 * ## Example configuration of a virtual source
 *
 * This example uses the rnnoise LADSPA plugin to create a new
 * virtual source.
 *
 * Run with `pipewire -c filter-chain.conf`. The configuration can also
 * be put under `pipewire.conf.d/` to run it inside the PipeWire server.
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/filter-chain.conf.d/my-filter-chain-1.conf
 *
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
 * # ~/.config/pipewire/filter-chain.conf.d/my-filter-chain-2.conf
 *
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

#define DEFAULT_RATE	48000

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct pw_properties *props;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

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

	struct spa_handle *handle;
	struct spa_filter_graph *graph;
	struct spa_hook graph_listener;
	uint32_t n_inputs;
	uint32_t n_outputs;
	bool graph_active;
};

static void capture_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->capture_listener);
	impl->capture = NULL;
}

static void capture_process(void *d)
{
	struct impl *impl = d;
	int res;
	if ((res = pw_stream_trigger_process(impl->playback)) < 0) {
		pw_log_debug("playback trigger error: %s", spa_strerror(res));
		while (true) {
			struct pw_buffer *t;
			if ((t = pw_stream_dequeue_buffer(impl->capture)) == NULL)
				break;
			/* playback part is not ready, consume, discard and recycle
			 * the capture buffers */
			pw_stream_queue_buffer(impl->capture, t);
		}
	}
}

static void playback_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	uint32_t i, data_size = 0;
	int32_t stride = 0;
	struct spa_data *bd;
	const void *cin[128];
	void *cout[128];

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

	for (i = 0; i < in->buffer->n_datas; i++) {
		uint32_t offs, size;

		bd = &in->buffer->datas[i];

		offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
		size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);

		cin[i] = SPA_PTROFF(bd->data, offs, void);

		data_size = i == 0 ? size : SPA_MIN(data_size, size);
		stride = SPA_MAX(stride, bd->chunk->stride);
	}
	for (; i < impl->n_inputs; i++)
		cin[i] = NULL;

	for (i = 0; i < out->buffer->n_datas; i++) {
		bd = &out->buffer->datas[i];

		data_size = SPA_MIN(data_size, bd->maxsize);

		cout[i] = bd->data;

		bd->chunk->offset = 0;
		bd->chunk->size = data_size;
		bd->chunk->stride = stride;
	}
	for (; i < impl->n_outputs; i++)
		cout[i] = NULL;

	pw_log_trace_fp("%p: stride:%d size:%d requested:%"PRIu64" (%"PRIu64")", impl,
			stride, data_size, out->requested, out->requested * stride);

	if (impl->graph_active)
		spa_filter_graph_process(impl->graph, cin, cout, data_size / sizeof(float));

done:
	if (in != NULL)
		pw_stream_queue_buffer(impl->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(impl->playback, out);
}

static int do_deactivate(struct spa_loop *loop, bool async, uint32_t seq,
                const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	impl->graph_active = false;
	return 0;
}

static int activate_graph(struct impl *impl)
{
	char rate[64];
	int res;

	if (impl->graph_active)
		return 0;

	snprintf(rate, sizeof(rate), "%lu", impl->rate);
	res = spa_filter_graph_activate(impl->graph, &SPA_DICT_ITEMS(
				SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE, rate)));

	if (res >= 0)
		impl->graph_active = true;

	return res;
}

static int deactivate_graph(struct impl *impl)
{
	struct pw_loop *data_loop;

	if (!impl->graph_active)
		return 0;

	data_loop = pw_stream_get_data_loop(impl->playback);
	pw_loop_invoke(data_loop, do_deactivate, 0, NULL, 0, true, impl);

	return spa_filter_graph_deactivate(impl->graph);
}

static int reset_graph(struct impl *impl)
{
	struct pw_loop *data_loop;
	int res;
	bool old_active = impl->graph_active;

	data_loop = pw_stream_get_data_loop(impl->playback);
	pw_loop_invoke(data_loop, do_deactivate, 0, NULL, 0, true, impl);

	res = spa_filter_graph_reset(impl->graph);

	impl->graph_active = old_active;

	return res;
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

static void capture_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(impl->capture, false);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("module %p: unconnected", impl);
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_info("module %p: error: %s", impl, error);
		break;
	case PW_STREAM_STATE_STREAMING:
	default:
		break;
	}
	return;
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
	int res;

	switch (id) {
	case SPA_PARAM_Format:
	{
		struct spa_audio_info_raw info;
		spa_zero(info);
		if (param == NULL) {
			pw_log_info("module %p: filter deactivate", impl);
			if (!capture)
				deactivate_graph(impl);
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
			spa_filter_graph_set_props(impl->graph,
					capture ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT, param);

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
	.state_changed = capture_state_changed,
	.param_changed = capture_param_changed
};

static void playback_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;
	int res;

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(impl->playback, false);
		reset_graph(impl);
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
			deactivate_graph(impl);
		}
		if ((res = activate_graph(impl)) < 0)
			goto error;
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
	.state_changed = playback_state_changed,
	.param_changed = playback_param_changed,
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t i, n_params, *offs;
	struct pw_array offsets;
	const struct spa_pod **params = NULL;
	struct spa_pod_dynamic_builder b;
	struct spa_filter_graph *graph = impl->graph;

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

	for (i = 0;; i++) {
		uint32_t save = b.b.state.offset;
		if (spa_filter_graph_enum_prop_info(graph, i, &b.b, NULL) != 1)
			break;
		if ((offs = pw_array_add(&offsets, sizeof(uint32_t))) != NULL)
			*offs = save;
	}

	if ((offs = pw_array_add(&offsets, sizeof(uint32_t))) != NULL)
		*offs = b.b.state.offset;
	spa_filter_graph_get_props(graph, &b.b, NULL);

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

static void copy_position(struct spa_audio_info_raw *dst, const struct spa_audio_info_raw *src)
{
	if (SPA_FLAG_IS_SET(dst->flags, SPA_AUDIO_FLAG_UNPOSITIONED) &&
	    !SPA_FLAG_IS_SET(src->flags, SPA_AUDIO_FLAG_UNPOSITIONED)) {
		for (uint32_t i = 0; i < src->channels; i++)
			dst->position[i] = src->position[i];
		SPA_FLAG_CLEAR(dst->flags, SPA_AUDIO_FLAG_UNPOSITIONED);
	}
}

static void graph_info(void *object, const struct spa_filter_graph_info *info)
{
	struct impl *impl = object;
	struct spa_dict *props = info->props;
	uint32_t i, val = 0;

	impl->n_inputs = info->n_inputs;
	impl->n_outputs = info->n_outputs;

	for (i = 0; props && i < props->n_items; i++) {
		const char *k = props->items[i].key;
		const char *s = props->items[i].value;
		pw_log_info("%s %s", k, s);
		if (spa_streq(k, "n_default_inputs") &&
		    impl->capture_info.channels == 0 &&
		    spa_atou32(s, &val, 0)) {
			pw_log_info("using default inputs %d", val);
			impl->capture_info.channels = val;
		}
		else if (spa_streq(k, "n_default_outputs") &&
		    impl->playback_info.channels == 0 &&
		    spa_atou32(s, &val, 0)) {
			pw_log_info("using default outputs %d", val);
			impl->playback_info.channels = val;
		}
	}
	if (impl->capture_info.channels == impl->playback_info.channels) {
		copy_position(&impl->capture_info, &impl->playback_info);
		copy_position(&impl->playback_info, &impl->capture_info);
	}
}

static void graph_apply_props(void *object, enum spa_direction direction, const struct spa_pod *props)
{
	struct impl *impl = object;
	pw_stream_set_param(direction == SPA_DIRECTION_INPUT ?
			impl->capture : impl->playback,
			SPA_PARAM_Props, props);
}

static void graph_props_changed(void *object, enum spa_direction direction)
{
	struct impl *impl = object;
	struct spa_filter_graph *graph = impl->graph;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	const struct spa_pod *params[1];

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
	spa_filter_graph_get_props(graph, &b.b, (struct spa_pod **)&params[0]);

	pw_stream_update_params(impl->capture, params, 1);
	spa_pod_dynamic_builder_clean(&b);
}

struct spa_filter_graph_events graph_events = {
	SPA_VERSION_FILTER_GRAPH_EVENTS,
	.info = graph_info,
	.apply_props = graph_apply_props,
	.props_changed = graph_props_changed,
};

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

	if (impl->handle)
		pw_unload_spa_handle(impl->handle);

	pw_properties_free(impl->capture_props);
	pw_properties_free(impl->playback_props);

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

static void parse_audio_info(struct pw_properties *props, struct spa_audio_info_raw *info)
{
	spa_audio_info_raw_init_dict_keys(info,
			&SPA_DICT_ITEMS(
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT, "F32P")),
			&props->dict,
			SPA_KEY_AUDIO_CHANNELS,
			SPA_KEY_AUDIO_POSITION, NULL);
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
	const struct pw_properties *p;
	struct pw_properties *props;
	struct impl *impl;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t pid = getpid();
	const char *str;
	int res;
	void *iface = NULL;

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
	impl->props = props;

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;

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

	p = pw_context_get_properties(impl->context);
	pw_properties_set(props, "clock.quantum-limit",
			pw_properties_get(p, "default.clock.quantum-limit"));

	pw_properties_setf(props, "filter-graph.n_inputs", "%d", impl->capture_info.channels);
	pw_properties_setf(props, "filter-graph.n_outputs", "%d", impl->playback_info.channels);

	pw_properties_set(props, SPA_KEY_LIBRARY_NAME, "filter-graph/libspa-filter-graph");
	impl->handle = pw_context_load_spa_handle(impl->context, "filter.graph", &props->dict);
	if (impl->handle == NULL) {
		res = -errno;
		goto error;
	}

	res = spa_handle_get_interface(impl->handle, SPA_TYPE_INTERFACE_FilterGraph, &iface);
	if (res < 0 || iface == NULL)
		goto error;

	impl->graph = iface;

	spa_filter_graph_add_listener(impl->graph, &impl->graph_listener,
			&graph_events, impl);

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

	setup_streams(impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
