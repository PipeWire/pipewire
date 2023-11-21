\page page_man_pipewire_1 pipewire

The PipeWire media server

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
