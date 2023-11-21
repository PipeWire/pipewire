\page page_man_pipewire-pulse_conf_5 pipewire-pulse.conf

The PipeWire Pulseaudio server configuration file

# SYNOPSIS

*$XDG_CONFIG_HOME/pipewire/pipewire-pulse.conf*

*$(PIPEWIRE_CONFIG_DIR)/pipewire-pulse.conf*

*$(PIPEWIRE_CONFDATADIR)/pipewire-pulse.conf*

*$(PIPEWIRE_CONFDATADIR)/pipewire-pulse.conf.d/*

*$(PIPEWIRE_CONFIG_DIR)/pipewire-pulse.conf.d/*

*$XDG_CONFIG_HOME/pipewire/pipewire-pulse.conf.d/*

# DESCRIPTION

Configuration for PipeWire's PulseAudio-compatible daemon.

The configuration file format is the same as for `pipewire.conf(5)`.
There are additional sections for configuring `pipewire-pulse(1)`
settings.

# CONFIGURATION FILE SECTIONS

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

In addition, the general PipeWire daemon configuration sections apply,
see \ref page_man_pipewire_conf_5 "pipewire.conf(5)".

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_module_protocol_pulse "libpipewire-module-protocol-pulse(7)",
\ref page_man_pipewire_conf_5 "pipewire.conf(5)",
\ref page_man_pipewire-pulse_1 "pipewire-pulse(1)",
\ref page_man_pipewire-pulse-modules_7 "pipewire-pulse-modules(7)"
