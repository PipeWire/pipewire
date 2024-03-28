\page page_man_pipewire_conf_5 pipewire.conf

The PipeWire server configuration file

\tableofcontents

# SYNOPSIS

*$XDG_CONFIG_HOME/pipewire/pipewire.conf*

*$(PIPEWIRE_CONFIG_DIR)/pipewire.conf*

*$(PIPEWIRE_CONFDATADIR)/pipewire.conf*

*$(PIPEWIRE_CONFDATADIR)/pipewire.conf.d/*

*$(PIPEWIRE_CONFIG_DIR)/pipewire.conf.d/*

*$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/*

# DESCRIPTION

PipeWire is a service that facilitates sharing of multimedia content
between devices and applications.

On startup, the daemon reads a main configuration file to configure
itself. It executes a series of commands listed in the config file.

The config file is looked up in the order listed in the
[SYNOPSIS](#synopsis). The environment variables `PIPEWIRE_CONFIG_DIR`,
`PIPEWIRE_CONFIG_PREFIX` and `PIPEWIRE_CONFIG_NAME` can be used to
specify an alternative config directory, subdirectory and file
respectively.

Other PipeWire configuration files generally follow the same lookup
logic, replacing `pipewire.conf` with the name of the particular
config file.

# DROP-IN CONFIGURATION FILES  @IDX@ pipewire.conf

All `*.conf` files in the `pipewire.conf.d/` directories are loaded
and merged into the configuration.  Dictionary sections are merged,
overriding properties if they already existed, and array sections are
appended to. The drop-in files have same format as the main
configuration file, but only contain the settings to be modified.

As the `pipewire.conf` configuration file contains various parts
that must be present for correct functioning, using drop-in files
for configuration is recommended.

## Example

A configuration file `~/.config/pipewire/pipewire.conf.d/custom.conf`
to change the value of the `default.clock.min-quantum` setting in `pipewire.conf`:

```css
context.properties = {
    default.clock.min-quantum = 128
}
```

# CONFIGURATION FILE FORMAT  @IDX@ pipewire.conf

The configuration file is in "SPA" JSON format.

The configuration file contains top-level keys, which are the sections.
The value of a section is either a dictionary, `{ }`, or an
array, `[ ]`. Section and dictionary item declarations 
have `KEY = VALUE` form, and are separated by whitespace.
For example:

```
context.properties = {  # top-level dictionary section

    key1 = value  # a simple value

    key2 = { key1 = value1 key2 = value2 }  # a dictionary with two entries

    key3 = [ value1 value2 ]  # an array with two entries

    key4 = [ { k = v1 } { k = v2 } ]  # an array of dictionaries

}

context.modules = [  # top-level array section

    value1

    value2

]
```

The configuration files can also be written in standard JSON syntax,
but for easier manual editing, the relaxed "SPA" variant is allowed.
In "SPA" JSON:

- `:` to delimit keys and values can be substituted by `=` or a space.
- <tt>\"</tt> around keys and string can be omitted as long as no special
  characters are used in the strings.
- `,` to separate objects can be replaced with a whitespace character.
- `#` can be used to start a comment until the line end

# CONFIGURATION FILE SECTIONS  @IDX@ pipewire.conf

\par context.properties
Dictionary. These properties configure the PipeWire instance.

\par context.spa-libs
Dictionary. Maps plugin features with globs to a spa library.

\par context.modules
Array of dictionaries. Each entry in the array is a dictionary with the
*name* of the module to load, including optional *args* and *flags*.
Most modules support being loaded multiple times.

\par context.objects
Array of dictionaries. Each entry in the array is a dictionary
containing the *factory* to create an object from and optional extra
arguments specific to that factory.

\par context.exec
\parblock
Array of dictionaries. Each entry in the array is dictionary containing
the *path* of a program to execute on startup and optional *args*.

This array used to contain an entry to start the session manager but
this mode of operation has since been demoted to development aid. Avoid
starting a session manager in this way in production environment.
\endparblock

\par node.rules
Array of dictionaries. Match rules for modifying node properties
on the server.

\par device.rules
Array of dictionaries. Match rules for modifying device properties
on the server.


# CONTEXT PROPERTIES  @IDX@ pipewire.conf

Available PipeWire properties in `context.properties` and possible
default values.

@PAR@ pipewire.conf  clock.power-of-two-quantum = true
The quantum requests from the clients and the final graph quantum are
rounded down to a power of two. A power of two quantum can be more
efficient for many processing tasks.

@PAR@ pipewire.conf  context.data-loop.library.name.system
The name of the shared library to use for the system functions for the data processing
thread. This can typically be changed if the data thread is running on a realtime
kernel such as EVL.

@PAR@ pipewire.conf  core.daemon = false
Makes the PipeWire process, started with this config, a daemon
process. This means that it will manage and schedule a graph for
clients. You would also want to configure a core.name to give it a
well known name.

@PAR@ pipewire.conf  core.name = pipewire-0
The name of the PipeWire context. This will also be the name of the
PipeWire socket clients can connect to.

@PAR@ pipewire.conf  cpu.zero.denormals = false
Configures the CPU to zero denormals automatically. This will be
enabled for the data processing thread only, when enabled.

@PAR@ pipewire.conf  default.clock.rate  = 48000
The default clock rate determines the real time duration of the
min/max/default quantums. You might want to change the quantums when
you change the default clock rate to maintain the same duration for
the quantums.

@PAR@ pipewire.conf  default.clock.allowed-rates = [ ]
It is possible to specify up to 32 alternative sample rates. The graph
sample rate will be switched when devices are idle. Note that this is
not enabled by default for now because of various kernel and Bluetooth
issues. Note that the min/max/default quantum values are scaled when
the samplerate changes.

@PAR@ pipewire.conf  default.clock.min-quantum = 32
Default minimum quantum.

@PAR@ pipewire.conf  default.clock.max-quantum = 8192
Default maximum quantum.

@PAR@ pipewire.conf  default.clock.quantum  = 1024
Default quantum used when no client specifies one.

@PAR@ pipewire.conf  default.clock.quantum-limit = 8192
Maximum quantum to reserve space for. This is the maximum buffer size used
in the graph, regardless of the samplerate.

@PAR@ pipewire.conf  default.clock.quantum-floor = 4
Minimum quantum to reserve space for. This is the minimum buffer size used
in the graph, regardless of the samplerate.

@PAR@ pipewire.conf  default.video.width
Default video width

@PAR@ pipewire.conf  default.video.height
Default video height

@PAR@ pipewire.conf  default.video.rate.num
Default video rate numerator

@PAR@ pipewire.conf  default.video.rate.denom
Default video rate denominator

@PAR@ pipewire.conf  library.name.system = support/libspa-support
The name of the shared library to use for the system functions for the main thread.

@PAR@ pipewire.conf  link.max-buffers = 64
The maximum number of buffers to negotiate between nodes. Note that version < 3 clients
can only support 16 buffers. More buffers is almost always worse than less, latency
and memory wise.

@PAR@ pipewire.conf  log.level = 2
The default log level used by the process.

@PAR@ pipewire.conf  mem.allow-mlock = true
Try to mlock the memory for the realtime processes. Locked memory will
not be swapped out by the kernel and avoid hickups in the processing
threads.

@PAR@ pipewire.conf  mem.warn-mlock = false
Warn about failures to lock memory. 

@PAR@ pipewire.conf  mem.mlock-all = false
Try to mlock all current and future memory by the process.

@PAR@ pipewire.conf  settings.check-quantum = false
Check if the quantum in the settings metadata update is compatible
with the configured limits.

@PAR@ pipewire.conf  settings.check-rate = false
Check if the rate in the settings metadata update is compatible
with the configured limits.

@PAR@ pipewire.conf  support.dbus = true
Enable DBus support. This will enable DBus support in the various modules that require
it. Disable this if you want to globally disable DBus support in the process.

@PAR@ pipewire.conf  vm.overrides = { default.clock.min-quantum = 1024 }
Any property in the vm.overrides property object will override the property
in the context.properties when PipeWire detects it is running in a VM.

@PAR@ pipewire.conf  context.modules.allow-empty = false
By default, a warning is logged when there are no context.modules loaded because this
likely indicates there is a problem. Some applications might load the modules themselves
and when they set this property to true, no warning will be logged.

The context properties may also contain custom values. For example,
the `context.modules` and `context.objects` sections can declare
additional conditions that control whether a module or object is loaded
depending on what properties are present.

# SPA LIBRARIES  @IDX@ pipewire.conf

SPA plugins are loaded based on their factory-name. This is a well
known name that uniquely describes the features that the plugin should
have. The `context.spa-libs` section provides a mapping between the
factory-name and the plugin where the factory can be found.

Factory names can contain a wildcard to group several related factories into one
plugin. The plugin is loaded from the first matching factory-name.

## Example

```json
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    avb.*           = avb/libspa-avb
    api.alsa.*      = alsa/libspa-alsa
    api.v4l2.*      = v4l2/libspa-v4l2
    api.libcamera.* = libcamera/libspa-libcamera
    api.bluez5.*    = bluez5/libspa-bluez5
    api.vulkan.*    = vulkan/libspa-vulkan
    api.jack.*      = jack/libspa-jack
    support.*       = support/libspa-support
    video.convert.* = videoconvert/libspa-videoconvert
}
```

# MODULES  @IDX@ pipewire.conf

PipeWire modules to be loaded. See
\ref page_man_libpipewire-modules_7 "libpipewire-modules(7)".

```json
context.modules = [
    #{ name = MODULENAME
    #    ( args  = { KEY = VALUE ... } )
    #    ( flags = [ ( ifexists ) ( nofail ) ] )
    #    ( condition = [ { KEY = VALUE ... } ... ] )
    #}
    #
]
```

\par name
Name of module to be loaded

\par args = { }
Arguments passed to the module

\par flags = [ ]
Loading flags. `ifexists` to only load module if it exists,
and `nofail` to not fail PipeWire startup if the module fails to load.

\par condition = [ ]
A \ref pipewire_conf__match_rules "match rule" `matches` condition.
The module is loaded only if one of the expressions in the array matches
to a context property.

# CONTEXT OBJECTS  @IDX@ pipewire.conf

The `context.objects` section allows you to make some objects from factories (usually created
by loading modules in `context.modules`).

```json
context.objects = [
    #{ factory = <factory-name>
    #    ( args  = { <key> = <value> ... } )
    #    ( flags = [ ( nofail ) ] )
    #    ( condition = [ { <key> = <value> ... } ... ] )
    #}
]
```
This section can be used to make nodes or links between nodes.

\par factory
Name of the factory to create the object.

\par args = { }
Arguments passed to the factory.

\par flags = [ ]
Flag `nofail` to not fail PipeWire startup if the object fails to load.

\par condition = [ ]
A \ref pipewire_conf__match_rules "match rule" `matches` condition.
The object is created only if one of the expressions in the array matches
to a context property.

## Example

This fragment creates a new dummy driver node, but only if
`core.daemon` property is true:

```json
context.objects = [
    { factory = spa-node-factory
      args = {
          factory.name    = support.node.driver
          node.name       = Dummy-Driver
          node.group      = pipewire.dummy
          priority.driver = 20000
      },
      condition = [ { core.daemon = true } ]
    }
]
```

# COMMAND EXECUTION  @IDX@ pipewire.conf

The `context.exec` section can be used to start arbitrary commands as
part of the initialization of the PipeWire program.

```json
context.exec = [
    #{   path = <program-name>
    #    ( args = "<arguments>" )
    #    ( condition = [ { <key> = <value> ... } ... ] )
    #}
]
```

\par path
Program to execute.

\par args
Arguments to the program.

\par condition
A \ref pipewire_conf__match_rules "match rule" `matches` condition.
The object is created only if one of the expressions in the array matches
to a context property.

## Example

The following fragment executes a pactl command with the given arguments:

```json
context.exec = [
    { path = "pactl" args = "load-module module-always-sink" }
]
```

# MATCH RULES  @IDX@ pipewire.conf

Some configuration file sections contain match rules. This makes it
possible to perform some action when an object (usually a node or
stream) is created/updated that matches certain properties.

The general rules object follows the following pattern:
```json
<rules> = [
    {
        matches = [
            # any of the following sets of properties are matched, if
            # any matches, the actions are executed
            {
                # <key> = <value>
                # all keys must match the value. ! negates. ~ starts regex.
                #application.process.binary = "teams"
                #application.name = "~speech-dispatcher.*"

                # Absence of property can be tested by comparing to null
                #pipewire.sec.flatpak = null
            }
            {
                # more matches here...
	    }
	    ...
        ]
        actions = {
            <action-name> = <action value>
	    ...
        }
    }
]
```

The rules is an array of things to match and what actions to perform
when a match is found.

The available actions and their values depend on the specific rule
that is used. Usually it is possible to update some properties or set
some quirks on the object.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-mon_1 "pw-mon(1)",
\ref page_man_libpipewire-modules_7 "libpipewire-modules(7)"
\ref page_man_pipewire-pulse_conf_5 "pipewire-pulse.conf(5)"
\ref page_man_pipewire-client_conf_5 "pipewire-client.conf(5)"
