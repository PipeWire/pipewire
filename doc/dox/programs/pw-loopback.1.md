\page page_man_pw-loopback_1 pw-loopback

PipeWire loopback client

# SYNOPSIS

**pw-loopback** \[*options*\]

# DESCRIPTION

The *pw-loopback* program is a PipeWire client that uses the PipeWire
loopback module to create loopback nodes, with configuration given via
the command-line options.

# OPTIONS

\par -h | \--help
Show help.

\par -r | \--remote=NAME
The name of the *remote* instance to connect to. If left unspecified, a
connection is made to the default PipeWire instance.

\par -n | \--name=NAME
Name of the loopback node

\par -g | \--group=NAME
Name of the loopback node group

\par -c | \--channels=NUMBER
Number of channels to provide

\par -m | \--channel-map=MAP
Channel map (default `[ FL, FR ]`)

\par -l | \--latency=LATENCY
Desired latency in ms

\par -d | \--delay=DELAY
Added delay in seconds (floating point allowed)

\par -C | \--capture=TARGET
Target device to capture from

\par -P | \--playback=TARGET
Target device to play to

\par \--capture-props=PROPS
Wanted properties of capture node (in JSON)

\par \--playback-props=PROPS
Wanted properties of capture node (in JSON)

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-cat_1 "pw-cat(1)",
**pactl(1)**

Other ways to create loopback nodes are adding the loopback module in
the configuration of a PipeWire daemon, or loading the loopback module
using Pulseaudio commands (`pactl load-module module-loopback ...`).
