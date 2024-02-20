\page page_man_pipewire-client_conf_5 client.conf

The PipeWire client configuration file.

\tableofcontents

# SYNOPSIS

*$XDG_CONFIG_HOME/pipewire/client.conf*

*$(PIPEWIRE_CONFIG_DIR)/client.conf*

*$(PIPEWIRE_CONFDATADIR)/client.conf*

*$(PIPEWIRE_CONFDATADIR)/client.conf.d/*

*$(PIPEWIRE_CONFIG_DIR)/client.conf.d/*

*$XDG_CONFIG_HOME/pipewire/client.conf.d/*

*$XDG_CONFIG_HOME/pipewire/client-rt.conf*

*$(PIPEWIRE_CONFIG_DIR)/client-rt.conf*

*$(PIPEWIRE_CONFDATADIR)/client-rt.conf*

*$(PIPEWIRE_CONFDATADIR)/client-rt.conf.d/*

*$(PIPEWIRE_CONFIG_DIR)/client-rt.conf.d/*

*$XDG_CONFIG_HOME/pipewire/client-rt.conf.d/*

# DESCRIPTION

Configuration for PipeWire native clients, and for PipeWire's ALSA
plugin.

A PipeWire native client program selects the default config to load,
and if nothing is specified, it usually loads `client.conf`.

The ALSA plugin uses the `client-rt.conf` file, as do some PipeWire
native clients such as \ref page_man_pw-cat_1 "pw-cat(1)".

The configuration file format and lookup logic is the same as for \ref page_man_pipewire_conf_5 "pipewire.conf(5)".

Drop-in configuration files `client.conf.d/*.conf` can be used, and are recommended.
See \ref pipewire_conf__drop-in_configuration_files "pipewire.conf(5)".

# CONFIGURATION FILE SECTIONS  @IDX@ client.conf

\par stream.properties
Configures options for native client streams.

\par stream.rules
Configures rules for native client streams.

\par alsa.properties
ALSA client configuration.

\par alsa.rules
ALSA client match rules.

In addition, the PipeWire context configuration sections 
may also be specified, see \ref page_man_pipewire_conf_5 "pipewire.conf(5)".

# STREAM PROPERTIES  @IDX@ client.conf

The client configuration files contain a stream.properties section that configures the options for client streams:
```
stream.properties = {
    #node.latency = 1024/48000
    #node.autoconnect = true
    #resample.disable = false
    #resample.quality = 4
    #monitor.channel-volumes = false
    #channelmix.disable = false
    #channelmix.min-volume = 0.0
    #channelmix.max-volume = 10.0
    #channelmix.normalize = false
    #channelmix.lock-volume = false
    #channelmix.mix-lfe = true
    #channelmix.upmix = true
    #channelmix.upmix-method = psd  # none, simple
    #channelmix.lfe-cutoff = 150.0
    #channelmix.fc-cutoff  = 12000.0
    #channelmix.rear-delay = 12.0
    #channelmix.stereo-widen = 0.0
    #channelmix.hilbert-taps = 0
    #dither.noise = 0
    #dither.method = none # rectangular, triangular, triangular-hf, wannamaker3, shaped5
    #debug.wav-path = ""
}
```

Some of the properties refer to different aspects of the stream:

* General stream properties to identify the stream.
* General stream properties to classify the stream.
* How it is going to be scheduled by the graph.
* How it is going to be linked by the session manager.
* How the internal processing will be done.
* Properties to configure the media format.

## Identifying Properties  @IDX@ client.conf

These contain properties to identify the node or to display the node in a GUI application.

@PAR@ client.conf  node.name
A (unique) name for the node. This is usually set on sink and sources to identify them
as targets for linking by the session manager.

@PAR@ client.conf  node.description
A human readable description of the node or stream.

@PAR@ client.conf  media.name
A user readable media name, usually the artist and title.
These are usually shown in user facing applications
to inform the user about the current playing media.

@PAR@ client.conf  media.title
A user readable stream title.

@PAR@ client.conf  media.artist
A user readable stream artist

@PAR@ client.conf  media.copyright
User readable stream copyright information

@PAR@ client.conf  media.software
User readable stream generator software information

@PAR@ client.conf  media.language
Stream language in POSIX format. Ex: `en_GB`

@PAR@ client.conf  media.filename
File name for the stream

@PAR@ client.conf  media.icon
Icon for the media, a base64 blob with PNG image data

@PAR@ client.conf  media.icon-name
An XDG icon name for the media. Ex: `audio-x-mp3`

@PAR@ client.conf  media.comment
Extra stream comment

@PAR@ client.conf  media.date
Date of the media

@PAR@ client.conf  media.format
User readable stream format information

@PAR@ client.conf  object.linger = false
If the object should outlive its creator.

## Classifying Properties  @IDX@ client.conf

The classifying properties of a node are use for routing the signal to its destination and
for configuring the settings.

@PAR@ client.conf  media.type
The media type contains a broad category of the media that is being processed by the node.
Possible values include "Audio", "Video", "Midi"

@PAR@ client.conf  media.category
\parblock
What kind of processing is done with the media. Possible values include:

* Playback: media playback.
* Capture: media capture.
* Duplex: media capture and playback or media processing in general.
* Monitor: a media monitor application. Does not actively change media data but monitors
           activity.
* Manager: Will manage the media graph.
\endparblock

@PAR@ client.conf  media.role
\parblock
The Use case of the media. Possible values include:

* Movie: Movie playback with audio and video.
* Music: Music listening.
* Camera: Recording video from a camera.
* Screen: Recording or sharing the desktop screen.
* Communication: VOIP or other video chat application.
* Game: Game.
* Notification: System notification sounds.
* DSP: Audio or Video filters and effect processing.
* Production: Professional audio processing and production.
* Accessibility: Audio and Visual aid for accessibility.
* Test: Test program.
\endparblock

@PAR@ client.conf  media.class
\parblock
The media class is to classify the stream function. Possible values include:

* Video/Source: a producer of video, like a webcam.
* Video/Sink: a consumer of video, like a display window.
* Audio/Source: a source of audio samples like a microphone.
* Audio/Sink: a sink for audio samples, like an audio card.
* Audio/Duplex: a node that is both a sink and a source.
* Stream/Output/Audio: a playback stream.
* Stream/Input/Audio: a capture stream.

The session manager assigns special meaning to the nodes based on the media.class. Sink or Source
classes are used as targets for Stream classes, etc..
\endparblock

## Scheduling Properties  @IDX@ client.conf

@PAR@ client.conf  node.latency = 1024/48000
Sets a suggested latency on the node as a fraction. This is just a suggestion, the graph will try to configure this latency or less for the graph. It is however possible that the graph is forced to a higher latency.

@PAR@ client.conf  node.lock-quantum = false
\parblock
When this node is active, the quantum of the graph is locked and not allowed to change automatically.
It can still be changed forcibly with metadata or when a node forces a quantum.

JACK clients use this property to avoid unexpected quantum changes.
\endparblock

@PAR@ client.conf  node.force-quantum = INTEGER
\parblock
While the node is active, force a quantum in the graph. The last node to be activated with this property wins.

A value of 0 unforces the quantum.
\endparblock

@PAR@ client.conf  node.rate = RATE
Suggest a rate (samplerate) for the graph. The suggested rate will only be applied when doing so would not cause
interruptions (devices are idle) and when the rate is in the list of allowed rates in the server.

@PAR@ client.conf  node.lock-rate = false
When the node is active, the rate of the graph will not change automatically. It is still possible to force a rate change with metadata or with a node property.

@PAR@ client.conf  node.force-rate = RATE
\parblock
When the node is active, force a specific sample rate on the graph. The last node to activate with this property wins.

A RATE of 0 means to force the rate in `node.rate` denominator.
\endparblock

@PAR@ client.conf  node.always-process = false
\parblock
When the node is active, it will always be joined with a driver node, even when nothing is linked to the node.
Setting this property to true also implies node.want-driver = true.

This is the default for JACK nodes, that always need their process callback called.
\endparblock

@PAR@ client.conf  node.want-driver = true
The node wants to be linked to a driver so that it can start processing. This is the default for streams
and filters since 0.3.51. Nodes that are not linked to anything will still be set to the idle state,
unless node.always-process is set to true.

@PAR@ client.conf  node.pause-on-idle = false
@PAR@ client.conf  node.suspend-on-idle = false
\parblock
When the node is not linked anymore, it becomes idle. Normally idle nodes keep processing and are suspended by the session manager after some timeout.  It is possible to immediately pause a node when idle with this property.

When the session manager does not suspend nodes (or when there is no session manager), the node.suspend-on-idle property can be used instead.
\endparblock

## Session Manager Properties  @IDX@ client.conf

@PAR@ client.conf  node.autoconnect = true
Instructs the session manager to automatically connect this node to some other node, usually
a sink or source.

@PAR@ client.conf  node.exclusive = false
If this node wants to be linked exclusively to the sink/source.

@PAR@ client.conf  node.target = <node.name|object.id>
Where this node should be linked to. This can be a node.name or an object.id of a node. This property is 
deprecated, the target.object property should be used instead, which uses the more unique object.serial as
a possible target.

@PAR@ client.conf  target.object = <node.name|object.serial>
Where the node should link to ths can be a node.name or an object.serial.

@PAR@ client.conf  node.dont-reconnect = false
\parblock
When the node has a target configured and the target is destroyed, destroy the node as well.
This property also inhibits that the node is moved to another sink/source.

Note that if a stream should appear/disappear in sync with the target, a session manager (WirePlumber) script
should be written instead.
\endparblock

@PAR@ client.conf  node.passive = false
\parblock
This is a passive node and so it should not keep sinks/sources busy. This property makes the session manager create passive links to the sink/sources. If the node is not otherwise linked (via a non-passive link), the node and the sink it is linked to are idle (and eventually suspended).

This is used for filter nodes that sit in front of sinks/sources and need to suspend together with the sink/source.
\endparblock

@PAR@ client.conf  node.link-group = ID
Add the node to a certain link group. Nodes from the same link group are not automatically linked to each other by the session manager. And example is a coupled stream where you don't want the output to link to the input streams, making a useless loop.

@PAR@ client.conf  stream.dont-remix = false
Instruct the session manager to not remix the channels of a stream. Normally the stream channel configuration is changed to match the sink/source it is connected to. With this property set to true, the stream will keep its original channel layout and the session manager will link matching channels with the sink.

## Audio Adapter Parameters  @IDX@ client.conf

An audio stream (and also audio device nodes) contain an audio adapter that can perform,
sample format, sample rate and channel mixing operations.

### Merger Parameters

The merger is used as the input for a sink device node or a capture stream. It takes the various channels and merges them into a single stream for further processing. 

The merger will also provide the monitor ports of the input channels and can
apply a software volume on the monitor signal.

@PAR@ client.conf  monitor.channel-volumes = false
The volume of the input channels is applied to the volume of the monitor ports. Normally
the monitor ports expose the raw unmodified signal on the input ports.

### Resampler Parameters

Source, sinks, capture and playback streams contain a high quality adaptive resampler.
It uses [sinc](https://ccrma.stanford.edu/~jos/resample/resample.pdf) based resampling
with linear interpolation of filter banks to perform arbitrary
resample factors. The resampler is activated in the following cases:

* The hardware of a device node does not support the graph samplerate. Resampling will occur
  from the graph samplerate to the hardware samplerate.
* The hardware clock of a device does not run at the same speed as the graph clock and adaptive
  resampling is required to match the clocks.
* A stream does not have the same samplerate as the graph and needs to be resampled.
* An application wants to activate adaptive resampling in a stream to make it match some other
  clock.

PipeWire performs most of the sample conversions and resampling in the client (Or in the case of the PulseAudio server, in the pipewire-pulse server that creates the streams). This ensures all the conversions are offloaded to the clients and the server can deal with one single format for performance reasons.

Below is an explanation of the options that can be tuned in the sample converter.

@PAR@ client.conf  resample.quality = 4
\parblock
The quality of the resampler. from 0 to 14, the default is 4.

Increasing the quality will result in better cutoff and less aliasing at the expense of
(much) more CPU consumption. The default quality of 4 has been selected as a good compromise
between quality and performance with no artifacts that are well below the audible range.

See [Infinite Wave](https://src.infinitewave.ca/) for a comparison of the performance.
\endparblock

@PAR@ client.conf  resample.disable = false
Disable the resampler entirely. The node will only be able to negotiate with the graph
when the samplerates are compatible.

### Channel Mixer Parameters

Source, sinks, capture and playback streams can apply channel mixing on the incoming signal.

Normally the channel mixer is not used for devices, the device channels are usually exposed as they are. This policy is usually enforced by the session manager, so we refer to its documentation there.

Playback and capture streams are usually configured to the channel layout of the sink/source
they connect to and will thus perform channel mixing.

The channel mixer also implements a software volume. This volume adjustment is performed on the original
channel layout. ex: A stereo playback stream that is up-mixed to 5.1 has 2 a left an right volume control.

@PAR@ client.conf  channelmix.disable = false
Disables the channel mixer completely. The stream will only be able to link to compatible
sources/sinks with the exact same channel layout.

@PAR@ client.conf  channelmix.min-volume = 0.0
@PAR@ client.conf  channelmix.max-volume = 10.0
Gives the min and max volume values allowed. Any volume that is set will be clamped to these
values.

@PAR@ client.conf  channelmix.normalize = false
\parblock
Makes sure that during such mixing & resampling original 0 dB level is preserved, so nothing sounds wildly quieter/louder.

While this options prevents clipping, it can in some cases produce too low volume. Increase the
volume in that case or disable normalization.
\endparblock

@PAR@ client.conf  channelmix.lock-volumes = false
Completely disable volume or mute changes. Defaults to false.

@PAR@ client.conf  channelmix.mix-lfe = true
Mixes the low frequency effect channel into the front center or stereo pair. This might enhance the dynamic range of the signal if there is no subwoofer and the speakers can reproduce the low frequency signal.

@PAR@ client.conf  channelmix.upmix = true
\parblock
Enables up-mixing of the front center (FC) when the target has a FC channel.
The sum of the stereo channels is used and an optional lowpass filter can be used
(see `channelmix.fc-cutoff`).

Also enabled up-mixing of LFE when `channelmix.lfe-cutoff` is set to something else than 0 and
the target has an LFE channel. The LFE channel is produced by adding the stereo channels.

If `channelmix.upmix` is true, the up-mixing of the rear channels is also enabled and controlled
with the `channelmix-upmix-method` property.
\endparblock

@PAR@ client.conf  channelmix.upmix-method = psd
\parblock
3 methods are provided to produce the rear channels in a surround sound:

1. none. No rear channels are produced.

2. simple. Front channels are copied to the rear. This is fast but can produce phasing effects.

3. psd. The rear channels as produced from the front left and right ambient sound (the
difference between the channels). A delay and optional phase shift are added to the rear signal
to make the sound bigger. 
\endparblock

@PAR@ client.conf  channelmix.lfe-cutoff = 150
Apply a lowpass filter to the low frequency effects. The value is expressed in Hz. Typical subwoofers have a cutoff at around 150 and 200. The default value of 0 disables the feature.

@PAR@ client.conf  channelmix.fc-cutoff = 12000
\parblock
Apply a lowpass filter to the front center frequency. The value is expressed in Hz.

Since the front center contains the dialogs, a typical cutoff frequency is 12000 Hz.

This option is only active when the up-mix is enabled.
\endparblock

@PAR@ client.conf  channelmix.rear-delay = 12.0
\parblock
Apply a delay in milliseconds when up-mixing the rear channels. This improves
spacialization of the sound. A typical delay of 12 milliseconds is the default.

This is only active when the `psd` up-mix method is used.
\endparblock

@PAR@ client.conf  channelmix.stereo-widen = 0.0
\parblock
Subtracts some of the front center signal from the stereo channels. This moves the dialogs
more to the center speaker and leaves the ambient sound in the stereo channels.

This is only active when up-mix is enabled and a Front Center channel is mixed.
\endparblock

@PAR@ client.conf  channelmix.hilbert-taps = 0
\parblock
This option will apply a 90 degree phase shift to the rear channels to improve spacialization.
Taps needs to be between 15 and 255 with more accurate results (and more CPU consumption)
for higher values.

This is only active when the `psd` up-mix method is used.
\endparblock

@PAR@ client.conf  dither.noise = 0
This option will add N bits of random data to the signal. This can be used
to keep some amplifiers alive during silent periods. This is usually used together with
`session.suspend-timeout-seconds` to disable suspend in the session manager.

@PAR@ client.conf  dither.method = none
\parblock
Optional [dithering](https://en.wikipedia.org/wiki/Dither) can be done on the quantized
output signal.

There are 6 modes available:

1. none           No dithering is done.
2. rectangular    Dithering with a rectangular noise distribution.
3. triangular     Dithering with a triangular noise distribution.
4. triangular-hf  Dithering with a sloped triangular noise distribution.
5. wannamaker3    Additional noise shaping is performed on the sloped triangular
                  dithering to move the noise to the more inaudible range. This is using
                  the "F-Weighted" noise filter described by Wannamaker.
6. shaped5        Additional noise shaping is performed on the triangular dithering
                  to move the noise to the more inaudible range. This is using the
                  Lipshitz filter.

Dithering is only useful for conversion to a format with less than 24 bits and will be
disabled otherwise.
\endparblock

## Debug Parameters

@PAR@ client.conf  debug.wav-path = ""
Make the stream to also write the raw samples to a WAV file for debugging puposes.

## Format Properties

Streams and also most device nodes can be configured in a certain format with properties.

@PAR@ client.conf  audio.rate = RATE
Forces a samplerate on the node.

@PAR@ client.conf  audio.channels = INTEGER
The number of audio channels to use. Must be a value between 1 and 64.

@PAR@ client.conf  audio.format = FORMAT
\parblock
Forces an audio format on the node. This is the format used internally in the node because the graph processing format is always float 32.

Valid formats include: S16, S32, F32, F64, S16LE, S16BE, ...
\endparblock

@PAR@ client.conf  audio.allowed-rates
An array of allowed samplerates for the node. ex. "[ 44100 48000 ]"

# STREAM RULES  @IDX@ client.conf

You can add \ref pipewire_conf__match_rules "match rules, see pipewire(1)"
to set properties for certain streams and filters.

`stream.rules` and `filter.rules` provides an `update-props` action
that takes an object with properties that are updated on the node
object of the stream and filter.

Add a `stream.rules` or `filter.rules` section in the config file like
this:

```
stream.rules = [
    {
        matches = [
            {
                # all keys must match the value. ! negates. ~ starts regex.
                application.process.binary = "firefox"
            }
        ]
        actions = {
            update-props = {
                node.name = "My Name"
            }
        }
    }
]
```

Will set the node.name of Firefox to "My Name".

# ALSA PROPERTIES  @IDX@ client.conf

An `alsa.properties` section can be added to configure ALSA specific client config.

```css
alsa.properties = {
    #alsa.deny = false
    #alsa.format = 0
    #alsa.rate = 0
    #alsa.channels = 0
    #alsa.period-bytes = 0
    #alsa.buffer-bytes = 0
    #alsa.volume-method = cubic         # linear, cubic
}
```

@PAR@ client.conf  alsa.deny
Denies ALSA access for the client. Useful in rules or PIPEWIRE_ALSA environment variable.

@PAR@ client.conf  alsa.format
The ALSA format to use for the client. This is an ALSA format name. default 0, which is to
allow all formats.

@PAR@ client.conf  alsa.rate
The samplerate to use for the client. The default is 0, which is to allow all rates.

@PAR@ client.conf  alsa.channels
The number of channels for the client. The default is 0, which is to allow any number of channels.

@PAR@ client.conf  alsa.period-bytes
The number of bytes per period. The default is 0 which is to allow any number of period bytes.

@PAR@ client.conf  alsa.buffer-bytes
The number of bytes in the alsa buffer. The default is 0, which is to allow any number of bytes.

@PAR@ client.conf  alsa.volume-method = cubic | linear
This controls the volume curve used on the ALSA mixer. Possible values are `cubic` and
`linear`. The default is to use `cubic`.

# ALSA RULES  @IDX@ client.conf

It is possible to set ALSA client specific properties by using
\ref pipewire_conf__match_rules "Match rules, see pipewire(1)". You can
set any of the above ALSA properties or any of the `stream.properties`.

### Example
```
alsa.rules = [
    {   matches = [ { application.process.binary = "resolve" } ]
        actions = {
            update-props = {
                alsa.buffer-bytes = 131072
            }
        }
    }
]
```

# ENVIRONMENT VARIABLES  @IDX@ client-env

See \ref page_man_pipewire_1 "pipewire(1)" for common environment
variables. Many of these also apply to client applications.

The environment variables also influence ALSA applications that are
using PipeWire's ALSA plugin.

@PAR@ client-env  PIPEWIRE_ALSA
\parblock
This can be an object with properties from `alsa.properties` or `stream.properties` that will
be used to construct the client and streams.

For example:
```
PIPEWIRE_ALSA='{ alsa.buffer-bytes=16384 node.name=foo }' aplay ...
```
Starts aplay with custom properties.
\endparblock

@PAR@ client-env  PIPEWIRE_NODE
\parblock
Instructs the ALSA client to link to a particular sink or source `object.serial` or `node.name`.

For example:
```
PIPEWIRE_NODE=alsa_output.pci-0000_00_1b.0.analog-stereo aplay ...
```
Makes aplay play on the give audio sink.
\endparblock

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_module_protocol_pulse "libpipewire-module-protocol-pulse(7)",
\ref page_man_pipewire_conf_5 "pipewire.conf(5)",
\ref page_man_pipewire-pulse_1 "pipewire-pulse(1)",
\ref page_man_pipewire-pulse-modules_7 "pipewire-pulse-modules(7)"
