\page page_man_pw-mididump_1 pw-mididump

The PipeWire MIDI dump

# SYNOPSIS

**pw-mididump** \[*options*\] \[*FILE*\]

# DESCRIPTION

Dump MIDI messages to stdout.

When a MIDI file is given, the events inside the file are printed.

When no file is given, **pw-mididump** creates a PipeWire MIDI input
stream and will print all MIDI events received on the port to stdout.

# OPTIONS

\par -r | \--remote=NAME
The name the remote instance to monitor. If left unspecified, a
connection is made to the default PipeWire instance.

\par -h | \--help
Show help.

\par \--version
Show version information.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-cat_1 "pw-cat(1)"
