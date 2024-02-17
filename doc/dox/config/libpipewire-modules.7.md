\page page_man_libpipewire-modules_7 libpipewire-modules

PipeWire modules

# DESCRIPTION

A PipeWire module is effectively a PipeWire client running inside
`pipewire(1)` which can host multiple modules. Usually modules are
loaded when they are listed in the configuration files. For example the
default configuration file loads several modules:

    context.modules = [
        ...
        # The native communication protocol.
        {   name = libpipewire-module-protocol-native }

        # The profile module. Allows application to access profiler
        # and performance data. It provides an interface that is used
        # by pw-top and pw-profiler.
        {   name = libpipewire-module-profiler }

        # Allows applications to create metadata objects. It creates
        # a factory for Metadata objects.
        {   name = libpipewire-module-metadata }

        # Creates a factory for making devices that run in the
        # context of the PipeWire server.
        {   name = libpipewire-module-spa-device-factory }
        ...
    ]

# KNOWN MODULES

$(LIBPIPEWIRE_MODULES)

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pipewire_conf_5 "pipewire.conf(5)"
