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

# DESCRIPTION

Configuration for PipeWire native clients, and for PipeWire's ALSA
plugin.

A PipeWire native client program selects the default config to load,
and if nothing is specified, it usually loads `client.conf`.

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

# STREAM PROPERTIES  @IDX@ client.conf stream.properties

The client configuration files contain a stream.properties section that configures the options for client streams:
```css
# ~/.config/pipewire/client.conf.d/custom.conf

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

A list of object properties that can be applied to streams can be found in
\ref props__common_node_properties "pipewire-props(7) Common Node Properties"
and
\ref props__audio_converter_properties "pipewire-props(7) Audio Adapter Properties"

# STREAM RULES  @IDX@ client.conf stream.rules

You can add \ref pipewire_conf__match_rules "match rules, see pipewire(1)"
to set properties for certain streams and filters.

`stream.rules` and `filter.rules` provides an `update-props` action
that takes an object with properties that are updated on the node
object of the stream and filter.

Add a `stream.rules` or `filter.rules` section in the config file like
this:

```css
# ~/.config/pipewire/client.conf.d/custom.conf

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

# ALSA CLIENT PROPERTIES  @IDX@ client.conf alsa.properties

An `alsa.properties` section can be added to configure client applications
that connect via the PipeWire ALSA plugin.

```css
# ~/.config/pipewire/client.conf.d/custom.conf

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

# ALSA CLIENT RULES  @IDX@ client.conf alsa.rules

It is possible to set ALSA client specific properties by using
\ref pipewire_conf__match_rules "Match rules, see pipewire(1)". You can
set any of the above ALSA properties or any of the `stream.properties`.

### Example

```css
# ~/.config/pipewire/client.conf.d/custom.conf

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
