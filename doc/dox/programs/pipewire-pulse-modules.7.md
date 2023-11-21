\page page_man_pipewire-pulse-modules_7 pipewire-pulse-modules

PipeWire Pulseaudio modules

# DESCRIPTION

PipeWire's Pulseaudio emulation implements several Pulseaudio modules.
It only supports its own built-in modules, and cannot load external
modules written for Pulseaudio.

The built-in modules can be loaded using Pulseaudio client programs, for
example `pactl load-module \<module-name\> \<module-options\>`.
They can also added to `pipewire-pulse.conf`, typically by a
drop-in file in `~/.config/pipewire/pipewire-pulse.conf.d/`
containing the module name and its arguments

    pulse.cmd = [
       { cmd = "load-module" args = "module-null-sink sink_name=foo" flags = [ ] }
    ]

# KNOWN MODULES

$(PIPEWIRE_PULSE_MODULES)

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire-pulse_1 "pipewire-pulse(1)"
