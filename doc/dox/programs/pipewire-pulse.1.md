\page page_man_pipewire-pulse_1 pipewire-pulse

The PipeWire PulseAudio replacement

# SYNOPSIS

**pipewire-pulse** \[*options*\]

# DESCRIPTION

**pipewire-pulse** starts a PulseAudio-compatible daemon that integrates
with the PipeWire media server, by running a pipewire process through a
systemd service. This daemon is a drop-in replacement for the PulseAudio
daemon.

# OPTIONS

\par -h | \--help
Show help.

\par -v | \--verbose
Increase the verbosity by one level. This option may be specified
multiple times.

\par \--version
Show version information.

\par -c | \--config=FILE
Load the given config file (Default: pipewire-pulse.conf).

# ENVIRONMENT VARIABLES

The generic \ref pipewire-env "pipewire(1) environment variables"
are supported.

In addition:

@PAR@ pulse-env  PULSE_RUNTIME_PATH

@PAR@ pulse-env  XDG_RUNTIME_DIR
Directory where to create the native protocol pulseaudio socket.

@PAR@ pulse-env  PULSE_LATENCY_MSEC
Extra buffering latency in milliseconds. This controls buffering
logic in `libpulse` and may be set for PulseAudio client applications
to adjust their buffering. (Setting it on the `pipewire-pulse` server
has no effect.)

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire-pulse_conf_5 "pipewire-pulse.conf(5)",
\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pipewire-pulse-modules_7 "pipewire-pulse-modules(7)"
