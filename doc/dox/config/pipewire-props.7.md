\page page_man_pipewire-props_7 pipewire-props

PipeWire object property reference.

\tableofcontents

# DESCRIPTION

PipeWire describes and configures audio and video elements with
objects of the following main types:

\par Node
Audio or video sink/source endpoint

\par Device
Sound cards, bluetooth devices, cameras, etc. May have multiple nodes.

\par Monitor
Finding devices and handling hotplugging

\par Port
Audio/video endpoint in a node

\par Link
Connection between ports, that transporting audio/video between them.

\par Client
Application connected to PipeWire.

All objects have *properties* ("props"), most of which can be set in
configuration files or at runtime when the object is created.

Some of the properties are "common properties" (for example
`node.description`) and can be set on all objects of the given
type. Other properties control settings of a specific kinds of device
or node (ALSA, Bluetooth, ...), and have meaning only for those
objects.

# CUSTOMIZING PROPERTIES  @IDX@ props

Usually, all device properties are configured in the session manager
configuration, see the session manager documentation.
Application properties are configured in
``client.conf`` (for native PipeWire and ALSA applications), and
``pipewire-pulse.conf`` (for Pulseaudio applications).

In minimal PipeWire setups without a session manager,
the device properties can be configured via
\ref pipewire_conf__context_objects "context.objects in pipewire.conf(5)"
when creating the devices.

\see [WirePlumber configuration](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration.html)

## Examples

Client configuration (requires client application restart to apply)
```css
# ~/.config/pipewire/client.conf/custom-props.conf

stream.rules = [
  {
    matches = [ { application.name = "pw-play" } ]
    actions = { update-props = { node.description = "Some pw-cat stream" } }
  }
]
```
\see \ref client_conf__stream_rules "pipewire-client.conf(5)", \ref client_conf__stream_rules "pipewire-pulse.conf(5)"

Device configuration (using WirePlumber; requires WirePlumber restart to apply):
```css
# ~/.config/wireplumber/wireplumber.conf.d/custom-props.conf

monitor.alsa.properties = {
  alsa.udev.expose-busy = true
}

monitor.alsa.rules = [
  {
    matches = [ { device.name = "~alsa_card.pci-.*" } ],
    actions = { update-props = { api.alsa.soft-mixer = true } ]
  },
  {
    matches = [ { node.name = "alsa_output.pci-0000_03_00.1.hdmi-stereo-extra3" } ]
    actions = { update-props = { node.description = "Main Audio" } ]
  }
]

monitor.bluez.properties = {
  bluez5.hfphsp-backend = ofono
}

monitor.bluez.rules = [
  {
    matches = [ { device.name = "~bluez_card.*" } ],
    actions = { update-props = { bluez5.dummy-avrcp player = true } ]
  }
]
```

\see [WirePlumber configuration](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration.html)

# COMMON DEVICE PROPERTIES  @IDX@ props

These are common properties for devices.

@PAR@ device-prop  device.name    # string
A (unique) name for the device. It can be used by command-line and other tools to identify the device.

@PAR@ device-prop  device.nick    # string
A short name for the device.

@PAR@ device-prop  device.param.PARAM = { ... }   # JSON
\parblock
Set value of a device \ref spa_param_type "Param" to a JSON value when the device is loaded.
This works similarly as \ref page_man_pw-cli_1 "pw-cli(1)" `set-param` command.
The `PARAM` should be replaced with the name of the Param to set,
ie. for example `device.Param.Props = { ... }` to set `Props`.
\endparblock

@PAR@ device-prop  device.plugged # integer
\parblock
\copydoc PW_KEY_DEVICE_PLUGGED
\endparblock

@PAR@ device-prop  device.nick # string
\parblock
\copydoc PW_KEY_DEVICE_NICK
\endparblock

@PAR@ device-prop  device.description # string
\parblock
\copydoc PW_KEY_DEVICE_DESCRIPTION
\endparblock

@PAR@ device-prop  device.serial # string
\parblock
\copydoc PW_KEY_DEVICE_SERIAL
\endparblock

@PAR@ device-prop  device.vendor.id # integer
\parblock
\copydoc PW_KEY_DEVICE_VENDOR_ID
\endparblock

@PAR@ device-prop  device.vendor.name # string
\parblock
\copydoc PW_KEY_DEVICE_VENDOR_NAME
\endparblock

@PAR@ device-prop  device.product.id # integer
\parblock
\copydoc PW_KEY_DEVICE_PRODUCT_NAME
\endparblock

@PAR@ device-prop  device.product.name # string
\parblock
\copydoc PW_KEY_DEVICE_PRODUCT_ID
\endparblock

@PAR@ device-prop  device.class # string
\parblock
\copydoc PW_KEY_DEVICE_CLASS
\endparblock

@PAR@ device-prop  device.form-factor # string
\parblock
\copydoc PW_KEY_DEVICE_FORM_FACTOR
\endparblock

@PAR@ device-prop  device.icon # string
\parblock
\copydoc PW_KEY_DEVICE_ICON
\endparblock

@PAR@ device-prop  device.icon-name # string
\parblock
\copydoc PW_KEY_DEVICE_ICON_NAME
\endparblock

@PAR@ device-prop  device.intended-roles # string
\parblock
\copydoc PW_KEY_DEVICE_INTENDED_ROLES
\endparblock

@PAR@ device-prop  device.disabled = false  # boolean
Disable the creation of this device in session manager.


There are other common `device.*` properties for technical purposes
and not usually user-configurable.

\see pw_keys in the API documentation for a full list.

# COMMON NODE PROPERTIES @IDX@ props

The properties here apply to general audio or video input/output
streams, and other nodes such as sinks or sources corresponding to
real or virtual devices.

## Identifying Properties  @IDX@ props

These contain properties to identify the node or to display the node in a GUI application.

@PAR@ node-prop  node.name    # string
A (unique) name for the node. This is usually set on sink and sources to identify them
as targets for linking by the session manager.

@PAR@ node-prop  node.nick    # string
A short name for the node.

@PAR@ node-prop  node.description    # string
A human readable description of the node or stream.

@PAR@ node-prop  media.name
A user readable media name, usually the artist and title.
These are usually shown in user facing applications
to inform the user about the current playing media.

@PAR@ node-prop  media.title
A user readable stream title.

@PAR@ node-prop  media.artist
A user readable stream artist

@PAR@ node-prop  media.copyright
User readable stream copyright information

@PAR@ node-prop  media.software
User readable stream generator software information

@PAR@ node-prop  media.language
Stream language in POSIX format. Ex: `en_GB`

@PAR@ node-prop  media.filename
File name for the stream

@PAR@ node-prop  media.icon
Icon for the media, a base64 blob with PNG image data

@PAR@ node-prop  media.icon-name
An XDG icon name for the media. Ex: `audio-x-mp3`

@PAR@ node-prop  media.comment
Extra stream comment

@PAR@ node-prop  media.date
Date of the media

@PAR@ node-prop  media.format
User readable stream format information

@PAR@ node-prop  object.linger = false
If the object should outlive its creator.

@PAR@ node-prop  device.id
ID of the device the node belongs to.

## Classifying Properties  @IDX@ props

The classifying properties of a node are use for routing the signal to its destination and
for configuring the settings.

@PAR@ node-prop  media.type
The media type contains a broad category of the media that is being processed by the node.
Possible values include "Audio", "Video", "Midi"

@PAR@ node-prop  media.category
\parblock
What kind of processing is done with the media. Possible values include:

* Playback: media playback.
* Capture: media capture.
* Duplex: media capture and playback or media processing in general.
* Monitor: a media monitor application. Does not actively change media data but monitors
           activity.
* Manager: Will manage the media graph.
\endparblock

@PAR@ node-prop  media.role
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

@PAR@ node-prop  media.class
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

## Scheduling Properties  @IDX@ props

@PAR@ node-prop  node.latency = 1024/48000
Sets a suggested latency on the node as a fraction. This is just a suggestion, the graph will try to configure this latency or less for the graph. It is however possible that the graph is forced to a higher latency.

@PAR@ node-prop  node.lock-quantum = false
\parblock
When this node is active, the quantum of the graph is locked and not allowed to change automatically.
It can still be changed forcibly with metadata or when a node forces a quantum.

JACK clients use this property to avoid unexpected quantum changes.
\endparblock

@PAR@ node-prop  node.force-quantum = INTEGER
\parblock
While the node is active, force a quantum in the graph. The last node to be activated with this property wins.

A value of 0 unforces the quantum.
\endparblock

@PAR@ node-prop  node.rate = RATE
Suggest a rate (samplerate) for the graph. The suggested rate will only be applied when doing so would not cause
interruptions (devices are idle) and when the rate is in the list of allowed rates in the server.

@PAR@ node-prop  node.lock-rate = false
When the node is active, the rate of the graph will not change automatically. It is still possible to force a rate change with metadata or with a node property.

@PAR@ node-prop  node.force-rate = RATE
\parblock
When the node is active, force a specific sample rate on the graph. The last node to activate with this property wins.

A RATE of 0 means to force the rate in `node.rate` denominator.
\endparblock

@PAR@ node-prop  node.always-process = false
\parblock
When the node is active, it will always be joined with a driver node, even when nothing is linked to the node.
Setting this property to true also implies node.want-driver = true.

This is the default for JACK nodes, that always need their process callback called.
\endparblock

@PAR@ node-prop  node.want-driver = true
The node wants to be linked to a driver so that it can start processing. This is the default for streams
and filters since 0.3.51. Nodes that are not linked to anything will still be set to the idle state,
unless node.always-process is set to true.

@PAR@ node-prop  node.pause-on-idle = false
@PAR@ node-prop  node.suspend-on-idle = false
\parblock
When the node is not linked anymore, it becomes idle. Normally idle nodes keep processing and are suspended by the session manager after some timeout.  It is possible to immediately pause a node when idle with this property.

When the session manager does not suspend nodes (or when there is no session manager), the node.suspend-on-idle property can be used instead.
\endparblock

@PAR@ node-prop  node.loop.name = null
@PAR@ node-prop  node.loop.class = data.rt
\parblock
Add the node to a specific loop name or loop class. By default the node is added to the
data.rt loop class. You can make more specific data loops and then assign the nodes to those.

Other well known names are main-loop.0 and the main node.loop.class which runs the node data processing
in the main loop.
\endparblock

@PAR@ node-prop  priority.driver    # integer
\parblock
The priority of choosing this device as the driver in the graph. The driver is selected from all linked devices by selecting the device with the highest priority.

Normally, the session manager assigns higher priority to sources so that they become the driver in the graph. The reason for this is that adaptive resampling should be done on the sinks rather than the source to avoid signal distortion when capturing audio.
\endparblock

@PAR@ node-prop  clock.name    # string
\parblock
The name of the clock. This name is auto generated from the card index and stream direction. Devices with the same clock name will not use a resampler to align the clocks. This can be used to link devices together with a shared word clock.

In Pro Audio mode, nodes from the same device are assumed to have the same clock and no resampling will happen when linked together. So, linking a capture port to a playback port will not use any adaptive resampling in Pro Audio mode.

In Non Pro Audio profile, no such assumption is made and adaptive resampling is done in all cases by default. This can also be disabled by setting the same clock.name on the nodes.
\endparblock

## Session Manager Properties  @IDX@ props

@PAR@ node-prop  node.autoconnect = true
Instructs the session manager to automatically connect this node to some other node, usually
a sink or source.

@PAR@ node-prop  node.exclusive = false
If this node wants to be linked exclusively to the sink/source.

@PAR@ node-prop  target.object = <node.name|object.serial>
Where the node should link to, this can be a node.name or an object.serial.

@PAR@ node-prop  node.target = <node.name|object.id>
Where this node should be linked to. This can be a node.name or an object.id of a node. This property is
deprecated, the target.object property should be used instead, which uses the more unique object.serial as
a possible target.

@PAR@ node-prop  node.dont-reconnect = false
\parblock
When the node has a target configured and the target is destroyed, destroy the node as well.
This property also inhibits that the node is moved to another sink/source.

Note that if a stream should appear/disappear in sync with the target, a session manager (WirePlumber) script
should be written instead.
\endparblock

@PAR@ node-prop  node.dont-fallback = false
If linking this node to its specified target does not succeed, session
manager should not fall back to linking it to the default target.

@PAR@ node-prop  node.dont-move = false
Whether the node target may be changed using metadata.

@PAR@ node-prop  node.passive = false
\parblock
This is a passive node and so it should not keep sinks/sources busy. This property makes the session manager create passive links to the sink/sources. If the node is not otherwise linked (via a non-passive link), the node and the sink it is linked to are idle (and eventually suspended).

This is used for filter nodes that sit in front of sinks/sources and need to suspend together with the sink/source.
\endparblock

@PAR@ node-prop  node.link-group = ID
Add the node to a certain link group. Nodes from the same link group are not automatically linked to each other by the session manager. And example is a coupled stream where you don't want the output to link to the input streams, making a useless loop.

@PAR@ node-prop  stream.dont-remix = false
Instruct the session manager to not remix the channels of a stream. Normally the stream channel configuration is changed to match the sink/source it is connected to. With this property set to true, the stream will keep its original channel layout and the session manager will link matching channels with the sink.

@PAR@ node-prop  priority.session    # integer
The priority for selecting this node as the default source or sink.

@PAR@ node-prop  session.suspend-timeout-seconds = 3  # float
Timeout in seconds, after which an idle node is suspended.
Value ``0`` means the node will not be suspended.

@PAR@ node-prop  state.restore-props = true
Whether session manager should save state for this node.

## Format Properties

Streams and also most device nodes can be configured in a certain format with properties.

@PAR@ node-prop  audio.rate = RATE
Forces a samplerate on the node.

@PAR@ node-prop  audio.channels = INTEGER
The number of audio channels to use. Must be a value between 1 and 64.

@PAR@ node-prop  audio.format = FORMAT
\parblock
Forces an audio format on the node. This is the format used internally in the node because the graph processing format is always float 32.

Valid formats include: S16, S32, F32, F64, S16LE, S16BE, ...
\endparblock

@PAR@ node-prop  audio.allowed-rates
An array of allowed samplerates for the node. ex. "[ 44100 48000 ]"

## Other Properties

@PAR@ node-prop  node.param.PARAM = { ... }   # JSON
\parblock
Set value of a node \ref spa_param_type "Param" to a JSON value when the device is loaded.
This works similarly as \ref page_man_pw-cli_1 "pw-cli(1)" `set-param` command.
The `PARAM` should be replaced with the name of the Param to set,
ie. for example `node.param.Props = { ... }` to set `Props`.
\endparblock

@PAR@ node-prop  node.disabled = false  # boolean
Disable the creation of this node in session manager.


# AUDIO ADAPTER PROPERTIES  @IDX@ props

Most audio nodes (ALSA, Bluetooth, audio streams from applications,
...) have common properties for the audio adapter. The adapter
performs sample format, sample rate and channel mixing operations.

All properties listed below are node properties.

## Merger Parameters

The merger is used as the input for a sink device node or a capture stream. It takes the various channels and merges them into a single stream for further processing.

The merger will also provide the monitor ports of the input channels and can
apply a software volume on the monitor signal.

@PAR@ node-prop  monitor.channel-volumes = false
The volume of the input channels is applied to the volume of the monitor ports. Normally
the monitor ports expose the raw unmodified signal on the input ports.

## Resampler Parameters

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

@PAR@ node-prop  resample.quality = 4
\parblock
The quality of the resampler. from 0 to 14, the default is 4.

Increasing the quality will result in better cutoff and less aliasing at the expense of
(much) more CPU consumption. The default quality of 4 has been selected as a good compromise
between quality and performance with no artifacts that are well below the audible range.

See [Infinite Wave](https://src.infinitewave.ca/) for a comparison of the performance.
\endparblock

@PAR@ node-prop  resample.disable = false
Disable the resampler entirely. The node will only be able to negotiate with the graph
when the samplerates are compatible.

## Channel Mixer Parameters

Source, sinks, capture and playback streams can apply channel mixing on the incoming signal.

Normally the channel mixer is not used for devices, the device channels are usually exposed as they are. This policy is usually enforced by the session manager, so we refer to its documentation there.

Playback and capture streams are usually configured to the channel layout of the sink/source
they connect to and will thus perform channel mixing.

The channel mixer also implements a software volume. This volume adjustment is performed on the original
channel layout. ex: A stereo playback stream that is up-mixed to 5.1 has 2 a left an right volume control.

@PAR@ node-prop  channelmix.disable = false
Disables the channel mixer completely. The stream will only be able to link to compatible
sources/sinks with the exact same channel layout.

@PAR@ node-prop  channelmix.min-volume = 0.0
@PAR@ node-prop  channelmix.max-volume = 10.0
Gives the min and max volume values allowed. Any volume that is set will be clamped to these
values.

@PAR@ node-prop  channelmix.normalize = false
\parblock
Makes sure that during such mixing & resampling original 0 dB level is preserved, so nothing sounds wildly quieter/louder.

While this options prevents clipping, it can in some cases produce too low volume. Increase the
volume in that case or disable normalization.
\endparblock

@PAR@ node-prop  channelmix.lock-volumes = false
Completely disable volume or mute changes. Defaults to false.

@PAR@ node-prop  channelmix.mix-lfe = true
Mixes the low frequency effect channel into the front center or stereo pair. This might enhance the dynamic range of the signal if there is no subwoofer and the speakers can reproduce the low frequency signal.

@PAR@ node-prop  channelmix.upmix = true
\parblock
Enables up-mixing of the front center (FC) when the target has a FC channel.
The sum of the stereo channels is used and an optional lowpass filter can be used
(see `channelmix.fc-cutoff`).

Also enabled up-mixing of LFE when `channelmix.lfe-cutoff` is set to something else than 0 and
the target has an LFE channel. The LFE channel is produced by adding the stereo channels.

If `channelmix.upmix` is true, the up-mixing of the rear channels is also enabled and controlled
with the `channelmix-upmix-method` property.
\endparblock

@PAR@ node-prop  channelmix.upmix-method = psd
\parblock
3 methods are provided to produce the rear channels in a surround sound:

1. none. No rear channels are produced.

2. simple. Front channels are copied to the rear. This is fast but can produce phasing effects.

3. psd. The rear channels as produced from the front left and right ambient sound (the
difference between the channels). A delay and optional phase shift are added to the rear signal
to make the sound bigger.
\endparblock

@PAR@ node-prop  channelmix.lfe-cutoff = 150
Apply a lowpass filter to the low frequency effects. The value is expressed in Hz. Typical subwoofers have a cutoff at around 150 and 200. The default value of 0 disables the feature.

@PAR@ node-prop  channelmix.fc-cutoff = 12000
\parblock
Apply a lowpass filter to the front center frequency. The value is expressed in Hz.

Since the front center contains the dialogs, a typical cutoff frequency is 12000 Hz.

This option is only active when the up-mix is enabled.
\endparblock

@PAR@ node-prop  channelmix.rear-delay = 12.0
\parblock
Apply a delay in milliseconds when up-mixing the rear channels. This improves
specialization of the sound. A typical delay of 12 milliseconds is the default.

This is only active when the `psd` up-mix method is used.
\endparblock

@PAR@ node-prop  channelmix.stereo-widen = 0.0
\parblock
Subtracts some of the front center signal from the stereo channels. This moves the dialogs
more to the center speaker and leaves the ambient sound in the stereo channels.

This is only active when up-mix is enabled and a Front Center channel is mixed.
\endparblock

@PAR@ node-prop  channelmix.hilbert-taps = 0
\parblock
This option will apply a 90 degree phase shift to the rear channels to improve specialization.
Taps needs to be between 15 and 255 with more accurate results (and more CPU consumption)
for higher values.

This is only active when the `psd` up-mix method is used.
\endparblock

@PAR@ node-prop  dither.noise = 0
\parblock
This option will add N bits of random data to the signal. When no dither.method is
specified, the random data will flip between [-(1<<(N-1)), 0] every 1024 samples. With
a dither.method, the dither noise is amplified with 1<<(N-1) bits.

This can be used to keep some amplifiers alive during silent periods. One or two bits of noise is
usually enough, otherwise the noise will become audible. This is usually used together with
`session.suspend-timeout-seconds` to disable suspend in the session manager.

Note that PipeWire uses floating point operations with 24 bits precission for all of the audio
processing. Conversion to 24 bits integer sample formats is lossless and conversion to 32 bits
integer sample formats are simply padded with 0 bits at the end. This means that the dither noise
is always only in the 24 most significant bits.
\endparblock

@PAR@ node-prop  dither.method = none
\parblock
Optional [dithering](https://en.wikipedia.org/wiki/Dither) can be done on the quantized
output signal.

There are 6 modes available:

1. none           No dithering is done.
2. rectangular    Dithering with a rectangular noise distribution. This adds random
                  bits in the [-0.5, 0.5] range to the signal with even distribution.
3. triangular     Dithering with a triangular noise distribution. This add random
                  bits in the [-1.0, 1.0] range to the signal with triangular distribution
                  around 0.0.
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

@PAR@ node-prop  debug.wav-path = ""
Make the stream to also write the raw samples to a WAV file for debugging purposes.

## Other Parameters

These control low-level technical features:

@PAR@ node-prop  clock.quantum-limit
\ref pipewire_conf__default_clock_quantum-limit "See pipewire.conf(5)"

@PAR@ node-prop  resample.peaks = false # boolean
Instead of actually resampling, produce peak amplitude values as output.
This is used for volume monitoring, where it is set as a property
of the "recording" stream.

@PAR@ node-prop  resample.prefill = false # boolean
Prefill resampler buffers with silence. This affects the initial
samples produced by the resampler.

@PAR@ node-prop  adapter.auto-port-config = null # JSON
\parblock
If specified, configure the ports of the node when it is created, instead of
leaving that to the session manager to do. This is useful (only) for minimal
configurations without a session manager.

Value is SPA JSON of the form:
```json
{
    mode = "none",          # "none", "passthrough", "convert", "dsp"
    monitor = false,        # boolean
    control = false,        # boolean
    position = "preserve"   # "unknown", "aux", "preserve"
}
```
See \ref spa_param_port_config for the meaning.
\endparblock

# ALSA PROPERTIES  @IDX@ props

## Monitor properties

@PAR@ monitor-prop  alsa.use-acp = true      # boolean
Use \ref monitor-prop__alsa_card_profiles "ALSA Card Profiles" (ACP) for device configuration.
This autodetects available ALSA devices and configures port and hardware mixers.

@PAR@ monitor-prop  alsa.udev.expose-busy    # boolean
Expose the ALSA card even if it is busy/in use. Default false. This can be useful when some
of the PCMs are in use by other applications but the other free PCMs should still be exposed.

## Device properties

@PAR@ device-prop  api.alsa.path    # string
ALSA device path as can be used in snd_pcm_open() and snd_ctl_open().

@PAR@ device-prop api.alsa.use-ucm = true  # boolean
\parblock
When ACP is enabled and a UCM configuration is available for a device, by
default it is used instead of the ACP profiles. This option allows you to
disable this and use the ACP profiles instead.

This option does nothing if `alsa.use-acp` is set to `false`.
\endparblock

@PAR@ device-prop  api.alsa.soft-mixer = false  # boolean
Setting this option to `true` will disable the hardware mixer for volume
control and mute. All volume handling will then use software volume and mute,
leaving the hardware mixer untouched. This can be interesting to work around
bugs in the mixer detection or decibel reporting. The hardware mixer will still
be used to mute unused audio paths in the device. Use `api.alsa.disable-mixer-path`
to also disable mixer path selection.

@PAR@ device-prop  api.alsa.disable-mixer-path = false  # boolean
Setting this option to `true` will disable the hardware mixer path selection.
The hardware mixer path is the configuration of the mixer depending on the
jacks that are inserted in the card. If this is disabled, you will have to
manually enable and disable mixer controls but it can be used to work around
bugs in the mixer. The hardware mixer will still be used for
volume and mute. Use `api.alsa.soft-mixer` to also disable hardware volume
and mute.

@PAR@ device-prop  api.alsa.ignore-dB = false  # boolean
Setting this option to `true` will ignore the decibel setting configured by
the driver. Use this when the driver reports wrong settings.

@PAR@ device-prop  device.profile-set  # string
This option can be used to select a custom ACP profile-set name for the
device. This can be configured in UDev rules, but it can also be specified
here. The default is to use "default.conf" unless there is a matching udev rule.

@PAR@ device-prop  device.profile  # string
The initial active profile name. The default is to start from the "Off"
profile and then let session manager select the best profile based on its
policy.

@PAR@ device-prop  api.acp.auto-profile = true  # boolean
Automatically select the best profile for the device. The session manager
usually disables this, as it handles this task instead. This can be
enabled in custom configurations without the session manager handling this.

@PAR@ device-prop  api.acp.auto-port = true  # boolean
Automatically select the highest priority port that is available ("port" is a
PulseAudio/ACP term, the equivalent of a "Route" in PipeWire). The session manager
usually disables this, as it handles this task instead. This can be
enabled in custom configurations without the session manager handling this.

@PAR@ device-prop  api.acp.probe-rate   # integer
Sets the samplerate used for probing the ALSA devices and collecting the
profiles and ports.

@PAR@ device-prop  api.acp.pro-channels  # integer
Sets the number of channels to use when probing the "Pro Audio" profile.
Normally, the maximum amount of channels will be used but with this setting
this can be reduced, which can make it possible to use other samplerates on
some devices.

@PAR@ device-prop  api.alsa.split-enable    # boolean
\parblock
\copydoc SPA_KEY_API_ALSA_SPLIT_ENABLE
\endparblock

## Node properties

@PAR@ node-prop  audio.channels    # integer
The number of audio channels to open the device with. Defaults depends on the profile of the device.

@PAR@ node-prop  audio.rate    # integer
The audio rate to open the device with. Default is 0, which means to open the device with a rate as close to the graph rate as possible.

@PAR@ node-prop  audio.format    # string
The audio format to open the device in. By default this is "UNKNOWN", which will open the device in the best possible bits (32/24/16/8..). You can force a format like S16_LE or S32_LE.

@PAR@ node-prop  audio.position    # JSON array of strings
The audio position of the channels in the device. This is auto detected based on the profile. You can configure an array of channel positions, like "[ FL, FR ]".

@PAR@ node-prop  audio.allowed-rates    # JSON array of integers
\parblock
The allowed audio rates to open the device with. Default is "[ ]", which means the device can be opened in any supported rate.

Only rates from the array will be used to open the device. When the graph is running with a rate not listed in the allowed-rates, the resampler will be used to resample to the nearest allowed rate.
\endparblock

@PAR@ node-prop  api.alsa.period-size    # integer
The period size to open the device in. By default this is 0, which will open the device in the default period size to minimize latency.

@PAR@ node-prop  api.alsa.period-num    # integer
The amount of periods to use in the device. By default this is 0, which means to use as many as possible.

@PAR@ node-prop  api.alsa.headroom    # integer
The amount of extra space to keep in the ringbuffer. The default is 0. Higher values can be configured when the device read and write pointers are not accurately reported.

@PAR@ node-prop  api.alsa.start-delay = 0  # integer
Some devices need some time before they can report accurate hardware pointer
positions. In those cases, an extra start delay can be added to compensate
for this startup delay. This sets the startup delay in samples.

@PAR@ node-prop  api.alsa.disable-mmap = false    # boolean
Disable mmap operation of the device and use the ALSA read/write API instead. Default is false, mmap is preferred.

@PAR@ node-prop  api.alsa.disable-batch    # boolean
Ignore the ALSA batch flag. If the batch flag is set, ALSA will need an extra period to update the read/write pointers. Ignore this flag from ALSA can reduce the latency. Default is false.

@PAR@ node-prop  api.alsa.use-chmap    # boolean
Use the driver provided channel map. Default is true when using UCM, false otherwise because many driver don't report this correctly.

@PAR@ node-prop  api.alsa.multi-rate    # boolean
Allow devices from the same card to be opened in multiple sample rates. Default is true. Some older drivers did not properly advertise the capabilities of the device and only really supported opening the device in one rate.

@PAR@ node-prop  api.alsa.htimestamp = false    # boolean
Use ALSA htimestamps in scheduling, instead of the system clock.
Some ALSA drivers produce bad timestamps, so this is not enabled by default
and will be disabled at runtime if it looks like the ALSA timestamps are bad.

@PAR@ node-prop  api.alsa.htimestamp.max-errors    # integer
Specify the number of consecutive errors before htimestamp is disabled.
Setting this to 0 makes htimestamp never get disabled.

@PAR@ node-prop  api.alsa.disable-tsched = false    # boolean
Disable timer-based scheduling, and use IRQ for scheduling instead.
The "Pro Audio" profile will usually enable this setting, if it is expected it works on the hardware.

@PAR@ node-prop  api.alsa.dll-bandwidth-max    # double
Sets the maximum bandwidth of the DLL (delay-locked loop) filter used to smooth out rate adjustments.
The default value may be too responsive in some scenarios.
For example, with UAC2 pitch control, the host reacts more slowly compared to local resampling,
so using a lower bandwidth helps avoid oscillations or instability.

@PAR@ node-prop  api.alsa.auto-link = false    # boolean
Link follower PCM devices to the driver PCM device when using IRQ-based scheduling.
The "Pro Audio" profile will usually enable this setting, if it is expected it works on the hardware.

@PAR@ node-prop  latency.internal.rate    # integer
Static set the device systemic latency, in samples at playback rate.

@PAR@ node-prop  latency.internal.ns    # integer
Static set the device systemic latency, in nanoseconds.

@PAR@ node-prop  api.alsa.path    # string
UNDOCUMENTED

@PAR@ node-prop  api.alsa.open.ucm    # boolean
Open device using UCM.

@PAR@ node-prop  api.alsa.bind-ctls    # boolean
UNDOCUMENTED

@PAR@ node-prop  iec958.codecs    # JSON array of string
Enable only specific IEC958 codecs. This can be used to disable some codecs the hardware supports.
Available values: PCM, AC3, DTS, MPEG, MPEG2-AAC, EAC3, TRUEHD, DTSHD

@PAR@ device-prop  api.alsa.split.parent    # boolean
\parblock
\copydoc SPA_KEY_API_ALSA_SPLIT_PARENT
\endparblock

@PAR@ node-prop  api.alsa.split.position  # JSON
\parblock
\copybrief SPA_KEY_API_ALSA_SPLIT_POSITION
Informative property.
\endparblock

@PAR@ node-prop  api.alsa.split.hw-position  # JSON
\parblock
\copybrief SPA_KEY_API_ALSA_SPLIT_HW_POSITION
Informative property.
\endparblock


# BLUETOOTH PROPERTIES  @IDX@ props

## Monitor properties

The following are settings for the Bluetooth device monitor, not device or
node properties:

@PAR@ monitor-prop  bluez5.roles = [ a2dp_sink a2dp_source bap_sink bap_source bap_bcast_sink bap_bcast_source hfp_hf hfp_ag ]   # JSON array of string
\parblock
Enabled roles.

Currently some headsets (Sony WH-1000XM3) are not working with
both hsp_ag and hfp_ag enabled, so by default we enable only HFP.

Supported roles:
- `hsp_hs` (HSP Headset),
- `hsp_ag` (HSP Audio Gateway),
- `hfp_hf` (HFP Hands-Free),
- `hfp_ag` (HFP Audio Gateway)
- `a2dp_sink` (A2DP Audio Sink)
- `a2dp_source` (A2DP Audio Source)
- `bap_sink` (LE Audio Basic Audio Profile Sink)
- `bap_source` (LE Audio Basic Audio Profile Source)
- `bap_bcast_sink` (LE Audio Basic Audio Profile Broadcast Sink)
- `bap_bcast_source` (LE Audio Basic Audio Profile Broadcast Source)
\endparblock

@PAR@ monitor-prop  bluez5.codecs   # JSON array of string
Enabled A2DP codecs (default: all).  Possible values: `sbc`, `sbc_xq`,
`aac`, `aac_eld`, `aptx`, `aptx_hd`, `aptx_ll`, `aptx_ll_duplex`,
`faststream`, `faststream_duplex`, `lc3plus_h3`, `ldac`, `opus_05`,
`opus_05_51`, `opus_05_71`, `opus_05_duplex`, `opus_05_pro`, `opus_g`,
`lc3`.

@PAR@ monitor-prop  bluez5.default.rate   # integer
Default audio rate.

@PAR@ monitor-prop  bluez5.default.channels   # integer
Default audio channels.

@PAR@ monitor-prop  bluez5.hfphsp-backend   # integer
HFP/HSP backend (default: native). Available values: any, none, hsphfpd, ofono, native

@PAR@ monitor-prop  bluez5.hfphsp-backend-native-modem   # string

@PAR@ monitor-prop  bluez5.dummy-avrcp player   # boolean
Register dummy AVRCP player. Some devices have wrongly functioning
volume or playback controls if this is not enabled. Default: false

@PAR@ monitor-prop  bluez5.enable-sbc-xq   # boolean
Override device quirk list and enable SBC-XQ for devices for which it is disabled.

@PAR@ monitor-prop  bluez5.enable-msbc   # boolean
Override device quirk list and enable MSBC for devices for which it is disabled.

@PAR@ monitor-prop  bluez5.enable-hw-volume   # boolean
Override device quirk list and enable hardware volume fo devices for which it is disabled.

@PAR@ monitor-prop  bluez5.hw-offload-sco   # boolean
\parblock
HFP/HSP hardware offload SCO support (default: false).

This feature requires a custom configuration that routes SCO audio to ALSA nodes,
in a platform-specific way. See `tests/examples/bt-pinephone.lua` in WirePlumber for an example.
Do not enable this setting if you don't know what all this means, as it won't work.
\endparblock

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.channels = 3   # integer
PipeWire Opus Pro audio profile channel count.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.coupled-streams = 1   # integer
PipeWire Opus Pro audio profile coupled stream count.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.locations = "FL,FR,LFE"   # string
PipeWire Opus Pro audio profile audio channel locations.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.max-bitrate = 600000   # integer
PipeWire Opus Pro audio profile max bitrate.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.frame-dms = 50   # integer
PipeWire Opus Pro audio profile frame duration (1/10 ms).

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.bidi.channels = 1   # integer
PipeWire Opus Pro audio profile duplex channels.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.bidi.coupled-streams = 0   # integer
PipeWire Opus Pro audio profile duplex coupled stream count.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.bidi.locations = "FC"   # string
PipeWire Opus Pro audio profile duplex coupled channel locations.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.bidi.max-bitrate = 160000   # integer
PipeWire Opus Pro audio profile duplex max bitrate.

@PAR@ monitor-prop  bluez5.a2dp.opus.pro.bidi.frame-dms = 400   # integer
PipeWire Opus Pro audio profile duplex frame duration (1/10 ms).

@PAR@ monitor-prop  bluez5.bcast_source.config = []  # JSON
\parblock
Example:
```
bluez5.bcast_source.config = [
  {
    "broadcast_code": "Børne House",
    "encryption: false,
    "sync_factor": 2,
    "bis": [
      { # BIS configuration
        "qos_preset": "16_2_1", # QOS preset name from table Table 6.4 from BAP_v1.0.1.
        "audio_channel_allocation": 1, # audio channel allocation configuration for the BIS
        "metadata": [ # metadata configurations for the BIS
           { "type": 1, "value": [ 1, 1 ] }
        ]
      }
    ]
  }
]
```
\endparblock

@PAR@ monitor-prop  bluez5.bap-server-capabilities.rates		# Array of integers
Supported sampling frequencies for the LC3 codec (default: all).
Possible values:
`8000`, `16000`, `24000`, `32000`, `44100`, `48000`

@PAR@ monitor-prop  bluez5.bap-server-capabilities.durations	# Array of doubles
Supported frame durations for the LC3 codec (default: all).
Possible values:
`7.5`, `10`

@PAR@ monitor-prop  bluez5.bap-server-capabilities.channels	# Array of integers
Supported audio channel counts for the LC3 codec (default: [1, 2]).
Possible values:
`1`, `2`, `3`, `4`, `5`, `6`, `7`, `8`

@PAR@ monitor-prop  bluez5.bap-server-capabilities.framelen_min		# integer
Minimum number of octets supported per codec frame for the LC3 codec (default: 20).

@PAR@ monitor-prop  bluez5.bap-server-capabilities.framelen_max		# integer
Maximum number of octets supported per codec frame for the LC3 codec (default: 400).

@PAR@ monitor-prop  bluez5.bap-server-capabilities.max_frames		# integer
Maximum number of codec frames supported per SDU for the LC3 codec (default: 2).

@PAR@ monitor-prop  bluez5.bap-server-capabilities.sink.locations		# JSON or integer
Sink audio locations of the server, as channel positions or PACS bitmask.
Example: `FL,FR`

@PAR@ monitor-prop  bluez5.bap-server-capabilities.sink.contexts		# integer
Available sink contexts PACS bitmask of the the server.

@PAR@ monitor-prop  bluez5.bap-server-capabilities.sink.supported-contexts		# integer
Supported sink contexts PACS bitmask of the the server.

@PAR@ monitor-prop  bluez5.bap-server-capabilities.source.locations		# JSON or integer
Source audio locations of the server, as channel positions or PACS bitmask.
Example: `FL,FR`

@PAR@ monitor-prop  bluez5.bap-server-capabilities.source.contexts		# integer
Available source contexts PACS bitmask of the the server.

@PAR@ monitor-prop  bluez5.bap-server-capabilities.source.supported-contexts		# integer
Supported source contexts PACS bitmask of the the server.

## Device properties

@PAR@ device-prop  bluez5.auto-connect   # boolean
Auto-connect devices on start up. Disabled by default if
the property is not specified.

@PAR@ device-prop  bluez5.hw-volume = [ hfp_ag hsp_ag a2dp_source ]  # JSON array of string
Profiles for which to enable hardware volume control.

@PAR@ device-prop  bluez5.profile   # string
Initial device profile. This usually has no effect as the session manager
overrides it.

@PAR@ device-prop  bluez5.a2dp.ldac.quality = "auto"   # string
LDAC encoding quality
Available values:
- auto (Adaptive Bitrate, default)
- hq   (High Quality, 990/909kbps)
- sq   (Standard Quality, 660/606kbps)
- mq   (Mobile use Quality, 330/303kbps)

@PAR@ device-prop  bluez5.a2dp.aac.bitratemode = 0   # integer
AAC variable bitrate mode.
Available values: 0 (cbr, default), 1-5 (quality level)

@PAR@ device-prop  bluez5.a2dp.opus.pro.application = "audio"   # string
PipeWire Opus Pro Audio encoding mode: audio, voip, lowdelay

@PAR@ device-prop  bluez5.a2dp.opus.pro.bidi.application = "audio"   # string
PipeWire Opus Pro Audio duplex encoding mode: audio, voip, lowdelay

@PAR@ device-prop  bluez5.bap.cig = "auto"   # integer, or 'auto'
Set CIG ID for BAP unicast streams of the device.

@PAR@ device-prop  bluez5.bap.preset = "auto"  # string
BAP QoS preset name that needed to be used with vendor config.
This property is experimental.
Available: "48_2_1", ... as in the BAP specification.

@PAR@ device-prop  bluez5.bap.rtn  # integer
BAP QoS preset name that needed to be used with vendor config.
This property is experimental.
Default: as per QoS preset.

@PAR@ device-prop  bluez5.bap.latency  # integer
BAP QoS latency that needs to be applied for vendor defined preset
This property is experimental.
Default: as QoS preset.

@PAR@ device-prop  bluez5.bap.delay = 40000 # integer
BAP QoS delay that needs to be applied for vendor defined preset
This property is experimental.
Default: as per QoS preset.

@PAR@ device-prop  bluez5.framing = false # boolean
BAP QoS framing that needs to be applied for vendor defined preset
This property is experimental.
Default: as per QoS preset.

## Node properties

@PAR@ node-prop  bluez5.media-source-role   # string
\parblock
Media source role for Bluetooth clients connecting to
this instance. Available values:
  - playback: playing stream to speakers
  - input: appear as source node.
\endparblock

@PAR@ node-prop  node.latency-offset-msec   # string
Applies only for BLE MIDI nodes.
Latency adjustment to apply on the node. Larger values add a
constant latency, but reduces timing jitter caused by Bluetooth
transport.

# PORT PROPERTIES  @IDX@ props

Port properties are usually not directly configurable via PipeWire
configuration files, as they are determined by applications creating
them. Below are some port properties may interesting for users:

@PAR@ port-prop  port.name # string
\parblock
\copydoc PW_KEY_PORT_NAME
\endparblock

@PAR@ port-prop  port.alias # string
\parblock
\copydoc PW_KEY_PORT_ALIAS
\endparblock

\see pw_keys in the API documentation for a full list.

# LINK PROPERTIES  @IDX@ props

Link properties are usually not directly configurable via PipeWire
configuration files, as they are determined by applications creating
them.

\see pw_keys in the API documentation for a full list.

# CLIENT PROPERTIES  @IDX@ props

Client properties are usually not directly configurable via PipeWire
configuration files, as they are determined by the application
connecting to PipeWire. Clients are however affected by the settings
in \ref page_man_pipewire_conf_5 "pipewire.conf(5)" and session
manager settings.

\note Only the properties `pipewire.*` are safe to use for security
purposes such as identifying applications and their capabilities, as
clients can set and change other properties freely.

Below are some client properties may interesting for users.

@PAR@ client-prop  application.name  # string
\parblock
\copydoc PW_KEY_APP_NAME
\endparblock

@PAR@ client-prop  application.process.id  # integer
\parblock
\copydoc PW_KEY_APP_PROCESS_ID
\endparblock

@PAR@ client-prop  pipewire.sec.pid  # integer
\parblock
Client pid, set by protocol.

Note that for PulseAudio applications, this is the PID of the
`pipewire-pulse` process.
\endparblock

\see pw_keys in the API documentation for a full list.

# RUNTIME SETTINGS  @IDX@ props

Objects such as devices and nodes also have *parameters* that can be
modified after the object has been created. For example, the active
device profile, channel volumes, and so on.

For some objects, the *parameters* also allow changing some of
the *properties*. The settings of most ALSA and virtual device parameters
can be configured also at runtime.

These settings are available in device *parameter* called `Props` in its
`params` field. They can be seen e.g. using `pw-dump <id>` for an ALSA device:

```json
{
...
      "Props": [
        {
          ...
          "params": [
              "audio.channels",
              2,
              "audio.rate",
              0,
              "audio.format",
              "UNKNOWN",
              "audio.position",
              "[ FL, FR ]",
              "audio.allowed-rates",
              "[  ]",
              "api.alsa.period-size",
              0,
              "api.alsa.period-num",
              0,
              "api.alsa.headroom",
              0,
              "api.alsa.start-delay",
              0,
              "api.alsa.disable-mmap",
              false,
              "api.alsa.disable-batch",
              false,
              "api.alsa.use-chmap",
              false,
              "api.alsa.multi-rate",
              true,
              "latency.internal.rate",
              0,
              "latency.internal.ns",
              0,
              "clock.name",
              "api.alsa.c-1"
            ]
          }
...
```

They generally have the same names and meaning as the corresponding properties.

One or more `params` can be changed using \ref page_man_pw-cli_1 "pw-cli(1)":
```
pw-cli s <id> Props '{ params = [ "api.alsa.headroom" 1024 ] }'
```
These settings are not saved and need to be reapplied for each session manager restart.

# ALSA CARD PROFILES  @IDX@ props

The sound card profiles ("Analog stereo", "Analog stereo duplex", ...) except "Pro Audio" come from two sources:

- UCM: ALSA Use Case Manager: the profile configuration system from ALSA. See https://github.com/alsa-project/alsa-ucm-conf/
- ACP ("Alsa Card Profiles"): Pulseaudio's profile system ported to PipeWire. See https://www.freedesktop.org/wiki/Software/PulseAudio/Backends/ALSA/Profiles/

See the above links on how to configure these systems.

For ACP, PipeWire looks for the profile configuration files under

- ~/.config/alsa-card-profile
- /etc/alsa-card-profile
- /usr/share/alsa-card-profile/mixer`.

The `path` and `profile-set` files are in subdirectories `paths` and `profile-sets` of these directories.
It is possible to override individual files locally by putting a modified copy into the ACP directories under `~/.config` or `/etc`.

# OTHER OBJECT TYPES  @IDX@ props

Technically, PipeWire objects is what are manipulated by applications
using the PipeWire API.

The list of object types that are usually "exported" (i.e. appear in
\ref page_man_pw-dump_1 "pw-dump(1)" output) is larger than considered
above:

- Node
- Device
- Port
- Link
- Client
- Metadata
- Module
- Profiler
- SecurityContext

Monitors do not appear in this list; they are not usually exported,
and technically also Device objects. They are considered above as a
separate object type because they have configurable properties.

Metadata objects are what is manipulated with
\ref page_man_pw-metadata_1 "pw-metadata(1)"

Modules can be loaded in configuration files, or by PipeWire
applications.

The Profiler and SecurityContext objects only provide corresponding
PipeWire APIs.

# INDEX {#pipewire-props__index}

## Monitor properties

@SECREF@ monitor-prop

## Device properties

@SECREF@ device-prop

## Node properties

@SECREF@ node-prop

## Port properties

@SECREF@ port-prop

## Client properties

@SECREF@ client-prop

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_conf_5 "pipewire.conf(5)"
