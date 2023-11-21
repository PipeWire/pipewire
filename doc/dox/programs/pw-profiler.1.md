\page page_man_pw-profiler_1 pw-profiler

The PipeWire profiler

# SYNOPSIS

**pw-profiler** \[*options*\]

# DESCRIPTION

Start profiling a PipeWire instance.

If the server has the profiler module loaded, this program will connect
to it and log the profiler data. Profiler data contains times and
durations when processing nodes and devices started and completed.

When this program is stopped, a set of **gnuplot** files and a script to
generate SVG files from the .plot files is generated, along with a .html
file to visualize the profiling results in a browser.

This function uses the same data used by *pw-top*.

# OPTIONS

\par -r | \--remote=NAME
The name the remote instance to monitor. If left unspecified, a
connection is made to the default PipeWire instance.

\par -h | \--help
Show help.

\par \--version
Show version information.

\par -o | \--output=FILE
Profiler output name (default "profiler.log").

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-top_1 "pw-top(1)"
