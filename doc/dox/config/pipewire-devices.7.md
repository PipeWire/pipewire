\page page_man_pipewire-devices_7 pipewire-devices

PipeWire device and node property reference.

\tableofcontents

# DESCRIPTION

Audio sinks and sources, cameras, Bluetooth endpoints, and other
objects have properties that can be set in configuration files or at
runtime.

Some of the properties are "common object properties" (e.g. such as
`node.description`) and can be set on all types of devices and
nodes. Other properties control settings of a specific type of a
device (ALSA, Bluetooth, ...).

All the properties are usually configured in the session manager configuration.
For how to configure them, see the session manager documentation.

In minimal PipeWire setups without a session manager, they can be configured via
\ref pipewire_conf__context_objects "context.objects in pipewire.conf(5)".

# RUNTIME SETTINGS  @IDX@ device-param

The settings of most ALSA and virtual device parameters can be
configured also at runtime.

The settings are available in device `Props` in the `params`
field. They can be seen e.g. using `pw-dump <id>` for an ALSA device:

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

One or more `params` can be changed using \ref page_man_pw-cli_1 "pw-cli(1)":
```
pw-cli s <id> Props '{ params = [ "api.alsa.headroom" 1024 ] }'
```
These settings are not saved and need to be reapplied for each session manager restart.

# COMMON NODE PROPERTIES  @IDX@ device-param

The properties listed in \ref client_conf__stream_properties "Stream properties"
apply also to sink or source nodes corresponding to real or virtual devices.

In addition:

@PAR@ device-param  priority.driver    # integer
\parblock
The priority of choosing this device as the driver in the graph. The driver is selected from all linked devices by selecting the device with the highest priority.

Normally, the session manager assigns higher priority to sources so that they become the driver in the graph. The reason for this is that adaptive resampling should be done on the sinks rather than the source to avoid signal distortion when capturing audio.
\endparblock

@PAR@ device-param  priority.session    # integer
The priority for selecting this device as the default device.

@PAR@ device-param  clock.name    # string
\parblock
The name of the clock. This name is auto generated from the card index and stream direction. Devices with the same clock name will not use a resampler to align the clocks. This can be used to link devices together with a shared word clock.

In Pro Audio mode, nodes from the same device are assumed to have the same clock and no resampling will happen when linked togther. So, linking a capture port to a playback port will not use any adaptive resampling in Pro Audio mode.

In Non Pro Audio profile, no such assumption is made and adaptive resampling is done in all cases by default. This can also be disabled by setting the same clock.name on the nodes.
\endparblock

@PAR@ device-param  device.id
ID of the device the node belongs to.

# COMMON DEVICE PROPERTIES  @IDX@ device-param

These are common properties for devices.

@PAR@ device-param  device.name    # string
A (unique) name for the device. It can be used by command-line and other tools to identify the device.

Other `device.*` properties: UNDOCUMENTED

# AUDIO CONVERTER PROPERTIES  @IDX@ device-param

Most audio nodes (ALSA, Bluetooth, ...) have common properties for the audio
converter. See \ref client_conf__stream_properties "pipewire-client.conf(5) stream.properties"
for explanations.

## Node properties

@PAR@ device-param  clock.quantum-limit
\ref pipewire_conf__default_clock_quantum-limit "See pipewire.conf(5)"

@PAR@ device-param  channelmix.disable
\ref client_conf__channelmix_disable "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.min-volume
\ref client_conf__channelmix_min-volume "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.max-volume
\ref client_conf__channelmix_max-volume "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.normalize
\ref client_conf__channelmix_normalize "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.mix-lfe
\ref client_conf__channelmix_mix-lfe "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.upmix
\ref client_conf__channelmix_upmix "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.lfe-cutoff
\ref client_conf__channelmix_lfe-cutoff "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.fc-cutoff
\ref client_conf__channelmix_fc-cutoff "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.rear-delay
\ref client_conf__channelmix_rear-delay "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.stereo-widen
\ref client_conf__channelmix_stereo-widen "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.hilbert-taps
\ref client_conf__channelmix_hilbert-taps "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.upmix-method
\ref client_conf__channelmix_upmix-method "See pipewire-client.conf(5)"

@PAR@ device-param  channelmix.lock-volumes
\ref client_conf__channelmix_lock-volumes "See pipewire-client.conf(5)"

@PAR@ device-param  resample.quality
\ref client_conf__resample_quality "See pipewire-client.conf(5)"

@PAR@ device-param  resample.disable
\ref client_conf__resample_disable "See pipewire-client.conf(5)"

@PAR@ device-param  resample.peaks
UNDOCUMENTED

@PAR@ device-param  resample.prefill
UNDOCUMENTED

@PAR@ device-param  monitor.channel-volumes
\ref client_conf__monitor_channel-volumes "See pipewire-client.conf(5)"

@PAR@ device-param  dither.noise
\ref client_conf__dither_noise "See pipewire-client.conf(5)"

@PAR@ device-param  dither.method
\ref client_conf__dither_method "See pipewire-client.conf(5)"

@PAR@ device-param  debug.wav-path
\ref client_conf__debug_wav-path "See pipewire-client.conf(5)"

@PAR@ device-param  adapter.auto-port-config
UNDOCUMENTED


# ALSA PROPERTIES  @IDX@ device-param

## Monitor properties

@PAR@ device-param  alsa.use-acp    # boolean
Use \ref device-param__alsa_card_profiles "ALSA Card Profiles" (ACP) for device configuration.

@PAR@ device-param  alsa.udev.expose-busy    # boolean
Expose the ALSA card even if it is busy/in use. Default false. This can be useful when some
of the PCMs are in use by other applications but the other free PCMs should still be exposed.

## Device properties

@PAR@ device-param  api.alsa.path    # string
ALSA device path as can be used in snd_pcm_open() and snd_ctl_open().

@PAR@ device-param  api.acp.auto-port    # boolean
Select reasonable port on device startup. Available for ACP devices.

@PAR@ device-param  api.acp.auto-profile    # boolean
Select reasonable profile on device startup. Available for ACP devices.

## Node properties

@PAR@ device-param  audio.channels    # integer
The number of audio channels to open the device with. Defaults depends on the profile of the device.

@PAR@ device-param  audio.rate    # integer
The audio rate to open the device with. Default is 0, which means to open the device with a rate as close to the graph rate as possible.

@PAR@ device-param  audio.format    # string
The audio format to open the device in. By default this is "UNKNOWN", which will open the device in the best possible bits (32/24/16/8..). You can force a format like S16_LE or S32_LE.

@PAR@ device-param  audio.position    # JSON array of strings
The audio position of the channels in the device. This is auto detected based on the profile. You can configure an array of channel positions, like "[ FL, FR ]".

@PAR@ device-param  audio.allowed-rates    # JSON array of integers
\parblock
The allowed audio rates to open the device with. Default is "[ ]", which means the device can be opened in any supported rate.

Only rates from the array will be used to open the device. When the graph is running with a rate not listed in the allowed-rates, the resampler will be used to resample to the nearest allowed rate.
\endparblock

@PAR@ device-param  api.alsa.period-size    # integer
The period size to open the device in. By default this is 0, which will open the device in the default period size to minimize latency.

@PAR@ device-param  api.alsa.period-num    # integer
The amount of periods to use in the device. By default this is 0, which means to use as many as possible.

@PAR@ device-param  api.alsa.headroom    # integer
The amount of extra space to keep in the ringbuffer. The default is 0. Higher values can be configured when the device read and write pointers are not accurately reported.

@PAR@ device-param  api.alsa.start-delay    # integer
Some devices require a startup period. The default is 0. Higher values can be set to send silence samples to the device first.

@PAR@ device-param  api.alsa.disable-mmap    # boolean
Disable mmap operation of the device and use the ALSA read/write API instead. Default is false, mmap is preferred.

@PAR@ device-param  api.alsa.disable-batch    # boolean
Ignore the ALSA batch flag. If the batch flag is set, ALSA will need an extra period to update the read/write pointers. Ignore this flag from ALSA can reduce the latency. Default is false.

@PAR@ device-param  api.alsa.use-chmap    # boolean
Use the driver provided channel map. Default is false because many drivers don't report this correctly.

@PAR@ device-param  api.alsa.multi-rate    # boolean
Allow devices from the same card to be opened in multiple sample rates. Default is true. Some older drivers did not properly advertize the capabilities of the device and only really supported opening the device in one rate.

@PAR@ device-param  api.alsa.htimestamp = false    # boolean
Use ALSA htimestamps in scheduling, instead of the system clock.
Some ALSA drivers produce bad timestamps, so this is not enabled by default
and will be disabled at runtime if it looks like the ALSA timestamps are bad.

@PAR@ device-param  api.alsa.htimestamp.max-errors    # integer
Specify the number of consecutive errors before htimestamp is disabled.
Setting this to 0 makes htimestamp never get disabled.

@PAR@ device-param  api.alsa.disable-tsched = false    # boolean
Disable timer-based scheduling, and use IRQ for scheduling instead.
The "Pro Audio" profile will usually enable this setting, if it is expected it works on the hardware.

@PAR@ device-param  api.alsa.auto-link = false    # boolean
Link follower PCM devices to the driver PCM device when using IRQ-based scheduling.
The "Pro Audio" profile will usually enable this setting, if it is expected it works on the hardware.

@PAR@ device-param  latency.internal.rate    # integer
Static set the device systemic latency, in samples at playback rate.

@PAR@ device-param  latency.internal.ns    # integer
Static set the device systemic latency, in nanoseconds.

@PAR@ device-param  api.alsa.path    # string
UNDOCUMENTED

@PAR@ device-param  api.alsa.open.ucm    # boolean
Open device using UCM.

@PAR@ device-param  api.alsa.bind-ctls    # boolean
UNDOCUMENTED

@PAR@ device-param  iec958.codecs    # JSON array of string
Enable only specific IEC958 codecs. This can be used to disable some codecs the hardware supports.
Available values: PCM, AC3, DTS, MPEG, MPEG2-AAC, EAC3, TRUEHD, DTSHD

# BLUETOOTH PROPERTIES  @IDX@ device-param

## Monitor properties

The following are settings for the Bluetooth device monitor, not device or
node properties:

@PAR@ device-param  bluez5.roles   # JSON array of string
\parblock
Enabled roles (default: [ a2dp_sink a2dp_source bap_sink bap_source hfp_hf hfp_ag ])

Currently some headsets (Sony WH-1000XM3) are not working with
both hsp_ag and hfp_ag enabled, so by default we enable only HFP.

Supported roles:
- hsp_hs (HSP Headset),
- hsp_ag (HSP Audio Gateway),
- hfp_hf (HFP Hands-Free),
- hfp_ag (HFP Audio Gateway)
- a2dp_sink (A2DP Audio Sink)
- a2dp_source (A2DP Audio Source)
- bap_sink (LE Audio Basic Audio Profile Sink)
- bap_source (LE Audio Basic Audio Profile Source)
\endparblock

@PAR@ device-param  bluez5.codecs   # JSON array of string
Enabled A2DP codecs (default: all).
Possible values: sbc sbc_xq aac aac_eld aptx aptx_hd aptx_ll aptx_ll_duplex faststream faststream_duplex lc3plus_h3 ldac opus_05 opus_05_51 opus_05_71 opus_05_duplex opus_05_pro opus_g lc3

@PAR@ device-param  bluez5.default.rate   # integer
Default audio rate.

@PAR@ device-param  bluez5.default.channels   # integer
Default audio channels.

@PAR@ device-param  bluez5.hfphsp-backend   # integer
HFP/HSP backend (default: native). Available values: any, none, hsphfpd, ofono, native

@PAR@ device-param  bluez5.hfphsp-backend-native-modem   # string

@PAR@ device-param  bluez5.dummy-avrcp player   # boolean
Register dummy AVRCP player. Some devices have wrongly functioning
volume or playback controls if this is not enabled. Default: false

@PAR@ device-param  bluez5.enable-sbc-xq   # boolean
Override device quirk list and enable SBC-XQ for devices for which it is disabled.

@PAR@ device-param  bluez5.enable-msbc   # boolean
Override device quirk list and enable MSBC for devices for which it is disabled.

@PAR@ device-param  bluez5.enable-hw-volume   # boolean
Override device quirk list and enable hardware volume fo devices for which it is disabled.

@PAR@ device-param  bluez5.hw-offload-sco   # boolean
\parblock
HFP/HSP hardware offload SCO support (default: false).

This feature requires a custom configuration that routes SCO audio to ALSA nodes,
in a platform-specific way. See `tests/examples/bt-pinephone.lua` in WirePlumber for an example.
Do not enable this setting if you don't know what all this means, as it won't work.
\endparblock

@PAR@ device-param  bluez5.a2dp.opus.pro.channels = 3   # integer
PipeWire Opus Pro audio profile channel count.

@PAR@ device-param  bluez5.a2dp.opus.pro.coupled-streams = 1   # integer
PipeWire Opus Pro audio profile coupled stream count.

@PAR@ device-param  bluez5.a2dp.opus.pro.locations = "FL,FR,LFE"   # string
PipeWire Opus Pro audio profile audio channel locations.

@PAR@ device-param  bluez5.a2dp.opus.pro.max-bitrate = 600000   # integer
PipeWire Opus Pro audio profile max bitrate.

@PAR@ device-param  bluez5.a2dp.opus.pro.frame-dms = 50   # integer
PipeWire Opus Pro audio profile frame duration (1/10 ms).

@PAR@ device-param  bluez5.a2dp.opus.pro.bidi.channels = 1   # integer
PipeWire Opus Pro audio profile duplex channels.

@PAR@ device-param  bluez5.a2dp.opus.pro.bidi.coupled-streams = 0   # integer
PipeWire Opus Pro audio profile duplex coupled stream count.

@PAR@ device-param  bluez5.a2dp.opus.pro.bidi.locations = "FC"   # string
PipeWire Opus Pro audio profile duplex coupled channel locations.

@PAR@ device-param  bluez5.a2dp.opus.pro.bidi.max-bitrate = 160000   # integer
PipeWire Opus Pro audio profile duplex max bitrate.

@PAR@ device-param  bluez5.a2dp.opus.pro.bidi.frame-dms = 400   # integer
PipeWire Opus Pro audio profile duplex frame duration (1/10 ms).

## Device properties

@PAR@ device-param  bluez5.auto-connect   # boolean
Auto-connect devices on start up. Disabled by default if
the property is not specified.

@PAR@ device-param  bluez5.hw-volume = [ PROFILE1 PROFILE2... ]   # JSON array of string
Profiles for which to enable hardware volume control (default: [ hfp_ag hsp_ag a2dp_source ]).

@PAR@ device-param  bluez5.profile   # string
Initial device profile. This usually has no effect as the session manager
overrides it.

@PAR@ device-param  bluez5.a2dp.ldac.quality   # string
LDAC encoding quality
Available values:
- auto (Adaptive Bitrate, default)
- hq   (High Quality, 990/909kbps)
- sq   (Standard Quality, 660/606kbps)
- mq   (Mobile use Quality, 330/303kbps)

@PAR@ device-param  bluez5.a2dp.aac.bitratemode   # integer
AAC variable bitrate mode.
Available values: 0 (cbr, default), 1-5 (quality level)

@PAR@ device-param  bluez5.a2dp.opus.pro.application = "audio"   # string
PipeWire Opus Pro Audio encoding mode: audio, voip, lowdelay

@PAR@ device-param  bluez5.a2dp.opus.pro.bidi.application = "audio"   # string
PipeWire Opus Pro Audio duplex encoding mode: audio, voip, lowdelay

## Node properties

@PAR@ device-param  bluez5.media-source-role   # string
\parblock
Media source role for Bluetooth clients connecting to
this instance. Available values:
  - playback: playing stream to speakers
  - input: appear as source node.
\endparblock

# ALSA CARD PROFILES  @IDX@ device-param

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

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_conf_5 "pipewire.conf(5)"
