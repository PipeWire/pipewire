\page page_man_pw-top_1 pw-top

The PipeWire process viewer

# SYNOPSIS

**pw-top** \[*options*\]

# DESCRIPTION

The *pw-top* program provides a dynamic real-time view of the pipewire
node and device statistics.

A hierarchical view is shown of Driver nodes and follower nodes. The
Driver nodes are actively using a timer to schedule dataflow in the
followers. The followers of a driver node as shown below their driver
with a + sign in a tree-like representation.

The columns presented are as follows:

\par S
\parblock
Node status.

- E = ERROR
- C = CREATING
- S = SUSPENDED
- I = IDLE
- R = RUNNING
\endparblock

\par ID
The ID of the pipewire node/device, as found in *pw-dump* and
*pw-cli*

\par QUANT
\parblock
The current quantum (for drivers) and the suggested quantum for
follower nodes.

The quantum by itself needs to be divided by the RATE column to
calculate the duration of a scheduling period in fractions of a
second.

For a QUANT of 1024 and a RATE of 48000, the duration of one period
in the graph is 1024/48000 or 21.3 milliseconds.

Follower nodes can have a 0 QUANT field, which means that the node
does not have a suggestion for the quantum and thus uses what the
driver selected.

The driver will use the lowest quantum of any of the followers. If
none of the followers select a quantum, the default quantum in the
pipewire configuration file will be used.

The QUANT on the drivers usually translates directly into the number
of audio samples processed per processing cycle of the graph.

See also
<https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/FAQ#pipewire-buffering-explained>
\endparblock

\par RATE
\parblock
The current rate (for drivers) and the suggested rate for follower
nodes.

This is the rate at which the *graph* processes data and needs to be
combined with the QUANT value to derive the duration of a processing
cycle in the graph.

Some nodes can have a 0 RATE, which means that they don\'t have any
rate suggestion for the graph. Nodes that suggest a rate can make
the graph switch rates if the graph is otherwise idle and the new
rate is allowed as a possible graph rate (see the pipewire
configuration file).

The RATE on (audio) driver nodes usually also translates directly to
the samplerate used by the device. Although some devices might not
be able to operate at the given samplerate, in which case resampling
will need to be done. The negotiated samplerate with the device and
stream can be found in the FORMAT column.

\endparblock

\par WAIT
\parblock
The waiting time of a node is the elapsed time between when the node
is ready to start processing and when it actually started
processing.

For Driver nodes, this is the time between when the node wakes up to
start processing the graph and when the driver (and thus also the
graph) completes a cycle. The WAIT time for driver is thus the
elapsed time processing the graph.

For follower nodes, it is the time spent between being woken up
(when all dependencies of the node are satisfied) and when
processing starts. The WAIT time for follower nodes is thus mostly
caused by context switching.

A value of \-\-- means that the node was not signaled. A value of
+++ means that the node was signaled but not awake.
\endparblock

\par BUSY
\parblock
The processing time is started when the node starts processing until
it completes and wakes up the next nodes in the graph.

A value of \-\-- means that the node was not started. A value of +++
means that the node was started but did not complete.
\endparblock

\par W/Q
\parblock
Ratio of WAIT / QUANT.

The W/Q time of the driver node is a good measure of the graph load.
The running averages of the driver W/Q ratios are used as the DSP
load in other (JACK) tools.

Values of \-\-- and +++ are copied from the WAIT column.

\endparblock

\par B/Q
\parblock
Ratio of BUSY / QUANT

This is a good measure of the load of a particular driver or
follower node.

Values of \-\-- and +++ are copied from the BUSY column.
\endparblock

\par ERR
\parblock
Total of Xruns and Errors

Xruns for drivers are when the graph did not complete a cycle. This
can be because a node in the graph also has an Xrun. It can also be
caused when scheduling delays cause a deadline to be missed, causing
a hardware Xrun.

Xruns for followers are incremented when the node started processing
but did not complete before the end of the graph cycle deadline.
\endparblock

\par FORMAT
\parblock
The format used by the driver node or the stream. This is the
hardware format negotiated with the device or stream.

If the stream of driver has a different rate than the graph,
resampling will be done.

For raw audio formats, the layout is \<sampleformat\> \<channels\>
\<samplerate\>.

For IEC958 passthrough audio formats, the layout is IEC958 \<codec\>
\<samplerate\>.

For DSD formats, the layout is \<dsd-rate\> \<channels\>.

For Video formats, the layout is \<pixelformat\>
\<width\>x\<height\>.
\endparblock

\par NAME
\parblock
Name assigned to the device/node, as found in *pw-dump* node.name

Names are prefixed by *+* when they are linked to a driver (entry
above with no +)
\endparblock


# OPTIONS

\par -h | \--help
Show help.

\par -b | \--batch-mode
Run in non-interactive batch mode, similar to top\'s batch mode.

\par -n | \--iterations=NUMBER
Exit after NUMBER of batch iterations. Only used in batch mode.

\par -r | \--remote=NAME
The name the *remote* instance to monitor. If left unspecified, a
connection is made to the default PipeWire instance.

\par -V | \--version
Show version information.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-dump_1 "pw-dump(1)",
\ref page_man_pw-cli_1 "pw-cli(1)",
\ref page_man_pw-profiler_1 "pw-profiler(1)"
