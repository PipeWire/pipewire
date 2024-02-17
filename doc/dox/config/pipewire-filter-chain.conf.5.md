\page page_man_pipewire-filter-chain_conf_5 filter-chain.conf

PipeWire example configuration for running audio filters.

\tableofcontents

# SYNOPSIS

*$XDG_CONFIG_HOME/pipewire/filter-chain.conf*

*$(PIPEWIRE_CONFIG_DIR)/filter-chain.conf*

*$(PIPEWIRE_CONFDATADIR)/filter-chain.conf*

*$(PIPEWIRE_CONFDATADIR)/filter-chain.conf.d/*

*$(PIPEWIRE_CONFIG_DIR)/filter-chain.conf.d/*

*$XDG_CONFIG_HOME/pipewire/filter-chain.conf.d/*

# DESCRIPTION

When \ref page_man_pipewire_1 "pipewire(1)" is run using
this configuration file, `pipewire -c filter-chain.conf`,
it starts a PipeWire client application that publishes
nodes that apply various audio filters to their input.

It is a normal PipeWire client application in all respects.

Drop-in configuration files `filter-chain.conf.d/*.conf` can be used
to modify the filter configuration, see \ref pipewire_conf__drop-in_configuration_files "pipewire.conf(5)".
Some examples are in *$(PIPEWIRE_CONFDATADIR)/filter-chain/*

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pipewire_conf_5 "pipewire.conf(5)"
