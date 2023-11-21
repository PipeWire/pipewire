\page page_man_pw-dump_1 pw-dump

The PipeWire state dumper

# SYNOPSIS

**pw-dump** \[*options*\]

# DESCRIPTION

The *pw-dump* program produces a representation of the current PipeWire
state as JSON, including the information on nodes, devices, modules,
ports, and other objects.

# OPTIONS

\par -h | \--help
Show help.

\par -r | \--remote=NAME
The name of the *remote* instance to dump. If left unspecified, a
connection is made to the default PipeWire instance.

\par -m | \--monitor
Monitor PipeWire state changes, and output JSON arrays describing
changes.

\par -N | \--no-colors
Disable color output.

\par -C | \--color=WHEN
Whether to enable color support. WHEN is `never`, `always`, or `auto`.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-cli_1 "pw-cli(1)",
\ref page_man_pw-top_1 "pw-top(1)",
