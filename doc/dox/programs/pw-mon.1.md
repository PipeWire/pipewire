\page page_man_pw-mon_1 pw-mon

The PipeWire monitor

# SYNOPSIS

**pw-mon** \[*options*\]

# DESCRIPTION

Monitor objects on the PipeWire instance.

# OPTIONS

\par -r | \--remote=NAME
The name the *remote* instance to monitor. If left unspecified, a
connection is made to the default PipeWire instance.

\par -h | \--help
Show help.

\par \--version
Show version information.

\par -N | \--color=WHEN
Whether to use color, one of 'never', 'always', or 'auto'. The default
is 'auto'. **-N** is equivalent to **--color=never**.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)"
