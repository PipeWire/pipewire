\page page_man_pipewire_1 pipewire

The PipeWire media server

\tableofcontents

# SYNOPSIS

**pipewire** \[*options*\]

# DESCRIPTION

PipeWire is a service that facilitates sharing of multimedia content
between devices and applications.

The **pipewire** daemon reads a config file that is further documented
in \ref page_man_pipewire_conf_5 "pipewire.conf(5)" manual page.

# OPTIONS

\par -h | \--help
Show help.

\par -v | \--verbose
Increase the verbosity by one level. This option may be specified
multiple times.

\par \--version
Show version information.

\par -c | \--config=FILE
Load the given config file (Default: pipewire.conf).

# RUNTIME SETTINGS  @IDX@ pipewire

A PipeWire daemon will also expose a settings metadata object that can
be used to change some settings at runtime.

Normally these settings can bypass any of the restrictions listed in
the config options above, such as quantum and samplerate values.

The settings can be modified using \ref page_man_pw-metadata_1 "pw-metadata(1)":
```
pw-metadata -n settings                  # list settings
pw-metadata -n settings 0                # list server settings
pw-metadata -n settings 0 log.level 2    # modify a server setting
```

@PAR@ pipewire-settings  log.level = INTEGER
Change the log level of the PipeWire daemon.

@PAR@ pipewire-settings  clock.rate = INTEGER
The default samplerate.

@PAR@ pipewire-settings  clock.allowed-rates = [ RATE1 RATE2... ]
The allowed samplerates.

@PAR@ pipewire-settings  clock.force-rate = INTEGER
\parblock
Temporarily forces the graph to operate in a fixed sample rate.
Both DSP processing and devices will switch to the new rate immediately.
Running streams (PulseAudio, native and ALSA applications) will automatically
resample to match the new rate.

Set the value to 0 to allow the sample rate to vary again.
\endparblock

@PAR@ pipewire-settings  clock.quantum = INTEGER
The default quantum (buffer size).

@PAR@ pipewire-settings  clock.min-quantum = INTEGER
Smallest quantum to be used.

@PAR@ pipewire-settings  clock.max-quantum = INTEGER
Largest quantum to be used.

@PAR@ pipewire-settings  clock.force-quantum = INTEGER
\parblock
Temporarily force the graph to operate in a fixed quantum.

Set the value to 0 to allow the quantum to vary again.
\endparblock

# ENVIRONMENT VARIABLES  @IDX@ pipewire-env

## Socket directories

@PAR@ pipewire-env PIPEWIRE_RUNTIME_DIR

@PAR@ pipewire-env XDG_RUNTIME_DIR

@PAR@ pipewire-env USERPROFILE
Used to find the PipeWire socket on the server (and native clients).

@PAR@ pipewire-env PIPEWIRE_CORE
Name of the socket to make.

@PAR@ pipewire-env PIPEWIRE_REMOTE
Name of the socket to connect to.

@PAR@ pipewire-env PIPEWIRE_DAEMON
If set to true then the process becomes a new PipeWire server.

## Config directories, config file name and prefix

@PAR@ pipewire-env PIPEWIRE_CONFIG_DIR

@PAR@ pipewire-env XDG_CONFIG_HOME

@PAR@ pipewire-env HOME
Used to find the config file directories.

@PAR@ pipewire-env PIPEWIRE_CONFIG_PREFIX

@PAR@ pipewire-env PIPEWIRE_CONFIG_NAME
Used to override the application provided
config prefix and config name.

@PAR@ pipewire-env PIPEWIRE_NO_CONFIG
Enables (false) or disables (true) overriding on the default configuration.

## Context information

As part of a client context, the following information is collected
from environment variables and placed in the context properties:

@PAR@ pipewire-env LANG
The current language in `application.language`.

@PAR@ pipewire-env XDG_SESSION_ID
Set as the `application.process.session-id` property.

@PAR@ pipewire-env DISPLAY
Is set as the `window.x11.display` property.

## Modules

@PAR@ pipewire-env PIPEWIRE_MODULE_DIR
Sets the directory where to find PipeWire modules.

@PAR@ pipewire-env SPA_SUPPORT_LIB
The name of the SPA support lib to load. This can be used to switch to
an alternative support library, for example, to run on the EVL realtime kernel.

## Logging options

@PAR@ pipewire-env JOURNAL_STREAM
Is used to parse the stream used for the journal. This is usually configured by
systemd.

@PAR@ pipewire-env PIPEWIRE_LOG_LINE
Enables the logging of line numbers. Default true.

@PAR@ pipewire-env PIPEWIRE_LOG
Specifies a log file to use instead of the default logger.

@PAR@ pipewire-env PIPEWIRE_LOG_SYSTEMD
Enables the use of systemd for the logger, default true.

## Other settings

@PAR@ pipewire-env PIPEWIRE_CPU
Selects the CPU and flags. This is a bitmask of any of the \ref CPU flags

@PAR@ pipewire-env PIPEWIRE_VM
Selects the Virtual Machine PipeWire is running on.  This can be any of the \ref CPU "VM"
types.

@PAR@ pipewire-env DISABLE_RTKIT
Disables the use of RTKit or the Realtime Portal for realtime scheduling.

@PAR@ pipewire-env NO_COLOR
Disables the use of colors in the console output.

## Debugging options

@PAR@ pipewire-env PIPEWIRE_DLCLOSE
Enables (true) or disables (false) the use of dlclose when a shared library
is no longer in use. When debugging, it might make sense to disable dlclose to be able to get
debugging symbols from the object.

## Stream options

@PAR@ pipewire-env PIPEWIRE_NODE
Makes a stream connect to a specific `object.serial` or `node.name`.

@PAR@ pipewire-env PIPEWIRE_PROPS
Adds extra properties to a stream or filter.

@PAR@ pipewire-env PIPEWIRE_QUANTUM
Forces a specific rate and buffer-size for the stream or filter.

@PAR@ pipewire-env PIPEWIRE_LATENCY
Sets a specific latency for a stream or filter. This is only a suggestion but
the configured latency will not be larger.

@PAR@ pipewire-env PIPEWIRE_RATE
Sets a rate for a stream or filter. This is only a suggestion. The rate will be
switched when the graph is idle.

@PAR@ pipewire-env PIPEWIRE_AUTOCONNECT
Overrides the default stream autoconnect settings.

## Plugin options

@PAR@ pipewire-env SPA_PLUGIN_DIR
Is used to locate SPA plugins.

@PAR@ pipewire-env SPA_DATA_DIR
Is used to locate plugin specific config files. This is used by the
bluetooth plugin currently to locate the quirks database.

@PAR@ pipewire-env SPA_DEBUG
Set the log level for SPA plugins. This is usually controlled by the `PIPEWIRE_DEBUG` variable
when the plugins are managed by PipeWire but some standalone tools (like spa-inspect) uses this
variable.

@PAR@ pipewire-env ACP_BUILDDIR
If set, the ACP profiles are loaded from the builddir.

@PAR@ pipewire-env ACP_PATHS_DIR

@PAR@ pipewire-env ACP_PROFILES_DIR
Used to locate the ACP paths and profile directories respectively.

@PAR@ pipewire-env LADSPA_PATH
Comma separated list of directories where the ladspa plugins can be found.

@PAR@ pipewire-env LIBJACK_PATH
Directory where the jack1 or jack2 libjack.so can be found.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pw-top_1 "pw-top(1)",
\ref page_man_pw-dump_1 "pw-dump(1)",
\ref page_man_pw-mon_1 "pw-mon(1)",
\ref page_man_pw-cat_1 "pw-cat(1)",
\ref page_man_pw-cli_1 "pw-cli(1)",
\ref page_man_libpipewire-modules_7 "libpipewire-modules(7)"
