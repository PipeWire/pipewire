\page page_man_spa-monitor_1 spa-monitor

The PipeWire SPA device debugging utility

# SYNOPSIS

**spa-monitor** *FILE*

# DESCRIPTION

Load a SPA plugin and instantiate a device from it.

This is only useful for debugging device plugins.

# EXAMPLES

**spa-monitor** $(SPA_PLUGINDIR)/jack/libspa-jack.so

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)"
