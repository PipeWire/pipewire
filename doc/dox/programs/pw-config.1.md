\page page_man_pw-config_1 pw-config

Debug PipeWire Config parsing

# SYNOPSIS

**pw-config** \[*options*\] paths

**pw-config** \[*options*\] list \[*SECTION*\]

**pw-config** \[*options*\] merge *SECTION*

# DESCRIPTION

List config paths and config sections and display the parsed output.

This tool can be used to get an overview of the config file that will be
parsed by the PipeWire server and clients.

# COMMON OPTIONS

\par -h | \--help
Show help.

\par \--version
Show version information.

\par -n | \--name=NAME
Config Name (default 'pipewire.conf')

\par -p | \--prefix=PREFIX
Config Prefix (default '')

\par -L | \--no-newline
Omit newlines after values

\par -r | \--recurse
Reformat config sections recursively

\par -N | \--no-colors
Disable color output

\par -C | \-color\[=WHEN\]
whether to enable color support. WHEN is
*never*, *always*, or *auto*

# LISTING PATHS

Specify the paths command. It will display all the config files that
will be parsed and in what order.

# LISTING CONFIG SECTIONS

Specify the list command with an optional *SECTION* to list the
configuration fragments used for *SECTION*. Without a *SECTION*, all
sections will be listed.

Use the -r options to reformat the sections.

# MERGING A CONFIG SECTION

With the merge option and a *SECTION*, pw-config will merge all config
files into a merged config section and dump the results. This will be
the section used by the client or server.

Use the -r options to reformat the sections.

# EXAMPLES

\par pw-config
List all config files that will be used

\par pw-config -n pipewire-pulse.conf
List all config files that will be used by the PipeWire pulseaudio
server.

\par pw-config -n pipewire-pulse.conf list
List all config sections used by the PipeWire pulseaudio server

\par pw-config -n jack.conf list context.properties
List the context.properties fragments used by the JACK clients

\par pw-config -n jack.conf merge context.properties
List the merged context.properties used by the JACK clients

\par pw-config -n pipewire.conf -r merge context.modules
List the merged context.modules used by the PipeWire server and reformat

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-dump_1 "pw-dump(1)",
