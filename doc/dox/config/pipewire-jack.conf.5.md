\page page_man_pipewire-jack_conf_5 jack.conf

The PipeWire JACK client configuration file.

\tableofcontents

# SYNOPSIS

*$XDG_CONFIG_HOME/pipewire/jack.conf*

*$(PIPEWIRE_CONFIG_DIR)/jack.conf*

*$(PIPEWIRE_CONFDATADIR)/jack.conf*

*$(PIPEWIRE_CONFDATADIR)/jack.conf.d/*

*$(PIPEWIRE_CONFIG_DIR)/jack.conf.d/*

*$XDG_CONFIG_HOME/pipewire/jack.conf.d/*

# DESCRIPTION

Configuration for PipeWire JACK clients.

The configuration file format and lookup logic is the same as for \ref page_man_pipewire_conf_5 "pipewire.conf(5)".

Drop-in configuration files `jack.conf.d/*.conf` can be used, and are recommended.
See \ref pipewire_conf__drop-in_configuration_files "pipewire.conf(5)".

# CONFIGURATION FILE SECTIONS  @IDX@ jack.conf

\par jack.properties
JACK client configuration.

\par jack.rules
JACK client match rules.

In addition, the PipeWire context configuration sections 
may also be specified, see \ref page_man_pipewire_conf_5 "pipewire.conf(5)".

# JACK PROPERTIES  @IDX@ jack.conf

The configuration file can contain an extra JACK specific section called `jack.properties` like this:
```
...
jack.properties = {
    #rt.prio             = 88
    #node.latency        = 1024/48000
    #node.lock-quantum   = true
    #node.force-quantum  = 0
    #jack.show-monitor   = true
    #jack.merge-monitor  = true
    #jack.show-midi      = true
    #jack.short-name     = false
    #jack.filter-name    = false
    #jack.filter-char    = " "
    #
    # allow:           Don't restrict self connect requests
    # fail-external:   Fail self connect requests to external ports only
    # ignore-external: Ignore self connect requests to external ports only
    # fail-all:        Fail all self connect requests
    # ignore-all:      Ignore all self connect requests
    #jack.self-connect-mode  = allow
    #jack.locked-process     = true
    #jack.default-as-system  = false
    #jack.fix-midi-events    = true
    #jack.global-buffer-size = false
    #jack.passive-links      = false
    #jack.max-client-ports   = 768
    #jack.fill-aliases       = false
    #jack.writable-input     = false

}
```

See `stream.properties` in 
\ref client_conf__stream_properties "pipewire-client.conf(5)" for
an explanation of the generic node properties.

It is also possible to have per-client settings, see Match Rules below.

@PAR@ jack.conf  rt.prio
To limit the realtime priority that jack clients can acquire.

@PAR@ jack.conf  node.latency
To force a specific minimum buffer size for the JACK applications, configure:
```
node.latency = 1024/48000
```
This configures a buffer-size of 1024 samples at 48KHz. If the graph is running at a different sample rate, the buffer-size will be adjusted accordingly.

@PAR@ jack.conf  node.lock-quantum
To make sure that no automatic quantum is changes while JACK applications are running, configure:
```
node.lock-quantum = true
```
The quantum can then only be changed by metadata or when an application is started with node.force-quantum. JACK Applications will also be able to use jack_set_buffersize() to override the quantum.

@PAR@ jack.conf  node.force-quantum
To force the quantum to a certain value and avoid changes to it:
```
    node.force-quantum = 1024
```
The quantum can then only be changed by metadata or when an application is started with node.force-quantum (or JACK applications that use jack_set_buffersize() to override the quantum).

@PAR@ jack.conf  jack.show-monitor
Show the Monitor client and its ports. 

@PAR@ jack.conf  jack.merge-monitor
\parblock
Exposes the capture ports and monitor ports on the same JACK device client. This is how JACK presents monitor ports to the clients. The default is however *not* to merge them together because this results in more user friendly user interfaces, usually. An extra client with a `Monitor` suffix is created that contains the monitor ports.

For example, this is (part of) the output of `jack_lsp` with the default setting (`jack.merge-monitor = false`):

Compare:

| `jack.merge-monitor = true` | `jack.merge-monitor = false` |
|:--|:--|
| Built-in Audio Analog Stereo:playback_FL | Built-in Audio Analog Stereo:playback_FL
| Built-in Audio Analog Stereo:monitor_FL | Built-in Audio Analog Stereo Monitor:monitor_FL
| Built-in Audio Analog Stereo:playback_FR | Built-in Audio Analog Stereo:playback_FR
| Built-in Audio Analog Stereo:monitor_FR |Built-in Audio Analog Stereo Monitor:monitor_FR
\endparblock

@PAR@ jack.conf  jack.show-midi
Show the MIDI clients and their ports.

@PAR@ jack.conf  jack.short-name
\parblock
To use shorter names for the device client names use `jack.short-name = true`. Compare:

| `jack.short-name = true` | `jack.short-name = false` |
|:--|:--|
| HDA Intel PCH:playback_FL | Built-in Audio Analog Stereo:playback_FL
| HDA Intel PCH Monitor:monitor_FL | Built-in Audio Analog Stereo Monitor:monitor_FL
| HDA Intel PCH:playback_FR | Built-in Audio Analog Stereo:playback_FR
| HDA Intel PCH Monitor:monitor_FR |Built-in Audio Analog Stereo Monitor:monitor_FR
\endparblock

@PAR@ jack.conf  jack.filter-name
@PAR@ jack.conf  jack.filter-char
Will replace all special characters with `jack.filter-char`. For clients the special characters are ` ()[].:*$` and for ports they are ` ()[].*$`. Use this option when a client is not able to deal with the special characters. (and older version of PortAudio was known to use the client and port names as a regex, and thus failing when there are regex special characters in the name).

@PAR@ jack.conf  jack.self-connect-mode
\parblock
Restrict a client from making connections to and from itself. Possible values and their meaning are summarized as:

| Value | Behavior
|:--|:--|
| `allow` | Don't restrict self connect requests.
| `fail-external` | Fail self connect requests to external ports only.
| `ignore-external` | Ignore self connect requests to external ports only.
| `fail-all` | Fail all self connect requests.
| `ignore-all` | Ignore all self connect requests.
\endparblock

@PAR@ jack.conf  jack.locked-process
Make sure the process and callbacks can not be called at the same time. This is the
normal operation but it can be disabled in case a specific client can handle this.

@PAR@ jack.conf  jack.default-as-system
\parblock
Name the default source and sink as `system` and number the ports to maximize
compatibility with JACK programs.

| `jack.default-as-system = false` | `jack.default-as-system = true` |
|:--|:--|
| HDA Intel PCH:playback_FL | system:playback_1
| HDA Intel PCH Monitor:monitor_FL | system:monitor_1
| HDA Intel PCH:playback_FR | system:playback_2
| HDA Intel PCH Monitor:monitor_FR | system:monitor_2
\endparblock

@PAR@ jack.conf  jack.fix-midi-events
Fix NoteOn events with a 0 velocity to NoteOff. This is standard behaviour in JACK and is thus
enabled by default to maximize compatibility. Especially LV2 plugins do not allow NoteOn
with 0 velocity.

@PAR@ jack.conf  jack.global-buffer-size
When a client has this option, buffersize changes will be applied globally and permanently for all PipeWire clients using the metadata.

@PAR@ jack.conf  jack.passive-links
Makes JACK clients make passive links. This option only works when the server link-factory was configured with the `allow.link.passive` option.

@PAR@ jack.conf  jack.max-client-ports
Limit the number of allowed ports per client to this value.

@PAR@ jack.conf  jack.fill-aliases
Automatically set the port alias1 and alias2 on the ports.

@PAR@ jack.conf  jack.writable-input
\parblock
Makes the input buffers writable. This is the default because some JACK clients write to the
input buffer. This however can cause corruption in other clients when they are also reading
from the buffer.

Set this to true to avoid buffer corruption if you are only dealing with non-buggy clients.
\endparblock

# MATCH RULES  @IDX@ jack.conf

`jack.rules` provides an `update-props` action that takes an object with properties that are updated
on the client and node object of the jack client.

Add a `jack.rules` section in the config file like this:

```
jack.rules = [
    {
        matches = [
            {
                # all keys must match the value. ! negates. ~ starts regex.
                application.process.binary = "jack_simple_client"
            }
        ]
        actions = {
            update-props = {
                node.latency = 512/48000
            }
        }
    }
    {
        matches = [
            {
                client.name = "catia"
            }
        ]
        actions = {
            update-props = {
                jack.merge-monitor = true
            }
        }
    }
]
```
Will set the latency of jack_simple_client to 512/48000 and makes Catia see the monitor client merged with the playback client.

# ENVIRONMENT VARIABLES  @IDX@ jack-env

See \ref page_man_pipewire_1 "pipewire(1)" for common environment
variables. Many of these also apply to JACK client applications.

Environment variables can be used to control the behavior of the PipeWire JACK client library.

@PAR@ jack-env  PIPEWIRE_NOJACK
@PAR@ jack-env  PIPEWIRE_INTERNAL
When any of these variables is set, the JACK client library will refuse to open a client. The `PIPEWIRE_INTERNAL` variable is set by the PipeWire main daemon to avoid self connections.

@PAR@ jack-env  PIPEWIRE_PROPS
Adds/overrides the properties specified in the `jack.conf` file. Check out the output of this:
```
> PIPEWIRE_PROPS='{ jack.short-name=true jack.merge-monitor=true }' jack_lsp
...
HDA Intel PCH:playback_FL
HDA Intel PCH:monitor_FL
HDA Intel PCH:playback_FR
HDA Intel PCH:monitor_FR
...
```

@PAR@ jack-env  PIPEWIRE_LATENCY
\parblock
```
PIPEWIRE_LATENCY=<samples>/<rate> <application>
```
A quick way to configure the maximum buffer-size for a client. It will run this client with the specified buffer-size (or smaller).

`PIPEWIRE_LATENCY=256/48000 jack_lsp` is equivalent to `PIPEWIRE_PROPS='{ node.latency=256/48000 }' jack_lsp`

A better way to start a jack session in a specific buffer-size is to force it with:
```
pw-metadata -n settings 0 clock.force-quantum <quantum>
```
This always works immediately and the buffer size will not change until the quantum is changed back to 0.
\endparblock

@PAR@ jack-env  PIPEWIRE_RATE
\parblock
```
PIPEWIRE_RATE=1/<rate> <application>
```

A quick way to configure the rate of the graph. It will try to switch the samplerate of the graph. This can usually only be done with the graph is idle and the rate is part of the allowed sample rates.

`PIPEWIRE_RATE=1/48000 jack_lsp` is equivalent to `PIPEWIRE_PROPS='{ node.rate=1/48000 }' jack_lsp`

A better way to start a jack session in a specific rate is to force the rate with:
```
pw-metadata -n settings 0 clock.force-rate <rate>
```
This always works and the samplerate does not need to be in the allowed rates. The rate will also not change until it is set back to 0.
\endparblock

@PAR@ jack-env  PIPEWIRE_QUANTUM
\parblock
```
PIPEWIRE_QUANTUM=<buffersize>/<rate> <application>
```

Is similar to using `PIPEWIRE_LATENCY=<buffersize>/<rate>` and `PIPEWIRE_RATE=1/<rate>` (see above), except that it is not just a suggestion but it actively *forces* the graph to change the rate and quantum. It can be used to set both a buffersize and samplerate at the same time.

When 2 applications force a quantum, the last one wins. When the winning app is stopped, the quantum of the previous app is restored.
\endparblock

@PAR@ jack-env  PIPEWIRE_LINK_PASSIVE
\parblock
```
PIPEWIRE_LINK_PASSIVE=true qjackctl
```
Make this client create passive links only. All links created by the client will be marked passive and will not keep the sink/source busy.

You can use this to link filters to devices. When there is no client connected to the filter, only passive links remain between the filter and the device and the device will become idle and suspended.
\endparblock

@PAR@ jack-env  PIPEWIRE_NODE
\parblock
```
PIPEWIRE_NODE=<id> <application>
```
Will sort the ports so that only the ports of the node with <id> are listed. You can use this to force an application to only deal with the ports of a certain node, for example when auto connecting.
\endparblock

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pw-jack_1 "pw-jack(1)",
\ref page_man_pipewire_conf_5 "pipewire.conf(5)"
