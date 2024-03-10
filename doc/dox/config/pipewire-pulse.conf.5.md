\page page_man_pipewire-pulse_conf_5 pipewire-pulse.conf

The PipeWire Pulseaudio server configuration file

\tableofcontents

# SYNOPSIS

*$XDG_CONFIG_HOME/pipewire/pipewire-pulse.conf*

*$(PIPEWIRE_CONFIG_DIR)/pipewire-pulse.conf*

*$(PIPEWIRE_CONFDATADIR)/pipewire-pulse.conf*

*$(PIPEWIRE_CONFDATADIR)/pipewire-pulse.conf.d/*

*$(PIPEWIRE_CONFIG_DIR)/pipewire-pulse.conf.d/*

*$XDG_CONFIG_HOME/pipewire/pipewire-pulse.conf.d/*

# DESCRIPTION

Configuration for PipeWire's PulseAudio-compatible daemon.

The configuration file format and lookup logic is the same as for \ref page_man_pipewire_conf_5 "pipewire.conf(5)".

Drop-in configuration files `pipewire-pulse.conf.d/*.conf` can be used, and are recommended.
See \ref pipewire_conf__drop-in_configuration_files "pipewire.conf(5)".

# CONFIGURATION FILE SECTIONS  @IDX@ pipewire-pulse.conf

\par stream.properties
Dictionary. These properties configure the PipeWire Pulseaudio server
properties.

\par stream.rules
Dictionary. These properties configure the PipeWire Pulseaudio server
properties.

\par pulse.properties
Dictionary. These properties configure the PipeWire Pulseaudio server
properties.

\par pulse.cmd
Array of dictionaries. A set of commands to be executed on startup.

\par pulse.rules
Array of dictionaries. A set of match rules and actions to apply to
clients.

See \ref page_module_protocol_pulse "libpipewire-module-protocol-pulse(7)"
for the detailed description.

In addition, the PipeWire context configuration sections 
may also be specified, see \ref page_man_pipewire_conf_5 "pipewire.conf(5)".

# STREAM PROPERTIES  @IDX@ pipewire-pulse.conf

The `stream.properties` section contains properties for streams created
by the pipewire-pulse server.

Available options are described in
\ref client_conf__stream_properties "pipewire-client.conf(5) stream.properties".

Some of these properties map to the PulseAudio `/etc/pulse/default.pa` config entries as follows:

| PulseAudio                     | PipeWire              | Notes                |
| ------------------------------ | --------------------- | -------------------- |
| remixing-use-all-sink-channels | channelmix.upmix      |                      |
| remixing-produce-lfe           | channelmix.lfe-cutoff | Set to > 0 to enable |
| remixing-consume-lfe           | channelmix.mix-lfe    |                      |
| lfe-crossover-freq             | channelmix.lfe-cutoff |                      |

## Example

```css
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
    #channelmix.mix-lfe = true
    #channelmix.upmix = true
    #channelmix.upmix-method = psd  # none, simple
    #channelmix.lfe-cutoff = 150.0
    #channelmix.fc-cutoff = 12000.0
    #channelmix.rear-delay = 12.0
    #channelmix.stereo-widen = 0.0
    #channelmix.hilbert-taps = 0
    #dither.noise = 0
    #dither.method = none # rectangular, triangular, triangular-hf, wannamaker3, shaped5
    #debug.wav-path = ""
}
```

# STREAM RULES  @IDX@ pipewire-pulse.conf

The `stream.rules` section works the same as
\ref client_conf__stream_rules "pipewire-client.conf(5) stream.rules".

# PULSEAUDIO PROPERTIES  @IDX@ pipewire-pulse.conf

For `pulse.properties` section,
see \ref page_module_protocol_pulse "libpipewire-module-protocol-pulse(7)" 
for available options.

# PULSEAUDIO RULES  @IDX@ pipewire-pulse.conf

For each client, a set of rules can be written in `pulse.rules`
section to configure quirks of the client or to force some pulse
specific stream configuration.

The general look of this section is as follows and follows the layout of
\ref pipewire_conf__match_rules "match rules, see pipewire(1)".

See \ref page_module_protocol_pulse "libpipewire-module-protocol-pulse(7)" 
for available options.

## Example

```css
pulse.rules = [
    {
        # skype does not want to use devices that don't have an S16 sample format.
        matches = [
             { application.process.binary = "teams" }
             { application.process.binary = "teams-insiders" }
             { application.process.binary = "skypeforlinux" }
        ]
        actions = { quirks = [ force-s16-info ] }
    }
    {
        # speech dispatcher asks for too small latency and then underruns.
        matches = [ { application.name = "~speech-dispatcher*" } ]
        actions = {
            update-props = {
                pulse.min.req          = 1024/48000     # 21ms
                pulse.min.quantum      = 1024/48000     # 21ms
            }
        }
    }
]
```

# PULSEAUDIO COMMANDS  @IDX@ pipewire-pulse.conf

As part of the server startup procedure you can execute some
additional commands with the `pulse.cmd` section in
`pipewire-pulse.conf`.

```css
...
pulse.cmd = [
    { cmd = "load-module" args = "module-always-sink" flags = [ ] }
    { cmd = "load-module" args = "module-switch-on-connect" }
    { cmd = "load-module" args = "module-gsettings" flags = [ "nofail" ] }
]
...
```

Additional commands can also be run via the
\ref pipewire_conf__context_exec "context.exec section, see pipewire.conf(5)".

Supported commands:

@PAR@ pipewire-pulse.conf load-module
Load the specified Pulseaudio module on startup, as if using **pactl(1)**
to load the module.

# PULSEAUDIO MODULES  @IDX@ pipewire-pulse.conf

For contents of section `pulse.modules`,
see \ref page_man_pipewire-pulse-modules_7 "pipewire-pulse-modules(7)".

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_module_protocol_pulse "libpipewire-module-protocol-pulse(7)",
\ref page_man_pipewire_conf_5 "pipewire.conf(5)",
\ref page_man_pipewire-pulse_1 "pipewire-pulse(1)",
\ref page_man_pipewire-pulse-modules_7 "pipewire-pulse-modules(7)"
