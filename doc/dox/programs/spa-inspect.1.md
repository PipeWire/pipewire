\page page_man_spa-inspect_1 spa-inspect

The PipeWire SPA plugin information utility

# SYNOPSIS

**spa-inspect** *FILE*

# DESCRIPTION

Displays information about a SPA plugin.

Lists the SPA factories contained, and tries to instantiate them.

# EXAMPLES

**spa-inspect** $(SPA_PLUGINDIR)/bluez5/libspa-codec-bluez5-sbc.so

Display information about a plugin.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)"
