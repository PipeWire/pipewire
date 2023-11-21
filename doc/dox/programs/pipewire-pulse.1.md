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

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire-pulse_conf_5 "pipewire-pulse.conf(5)",
\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pipewire-pulse-modules_7 "pipewire-pulse-modules(7)"
