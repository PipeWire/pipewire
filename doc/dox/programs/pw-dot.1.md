\page page_man_pw-dot_1 pw-dot

The PipeWire dot graph dump

# SYNOPSIS

**pw-dot** \[*options*\]

# DESCRIPTION

Create a .dot file of the PipeWire graph.

The .dot file can then be visualized with a tool like **dotty** or
rendered to a PNG file with `dot -Tpng pw.dot -o pw.png`.

# OPTIONS

\par -r | \--remote=NAME
The name the remote instance to connect to. If left unspecified, a
connection is made to the default PipeWire instance.

\par -h | \--help
Show help.

\par \--version
Show version information.

\par -a | \--all
Show all object types.

\par -s | \--smart
Show linked objects only.

\par -d | \--detail
Show all object properties.

\par -o FILE | \--output=FILE
Output file name (Default pw.dot). Use - for stdout.

\par -L | \--lr
Lay the graph from left to right, instead of dot's default top to
bottom.

\par -9 | \--90
Lay the graph using 90-degree angles in edges.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-cli_1 "pw-cli(1)",
\ref page_man_pw-mon_1 "pw-mon(1)",
