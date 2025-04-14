\page page_man_pw-link_1 pw-link

The PipeWire Link Command

# SYNOPSIS

**pw-link** \[*options*\] -o-l \[*out-pattern*\] \[*in-pattern*\]

**pw-link** \[*options*\] *output* *input*

**pw-link** \[*options*\] -d *output* *input*

**pw-link** \[*options*\] -d *link-id*

# DESCRIPTION

List, create and destroy links between PipeWire ports.

# COMMON OPTIONS

\par -r | \--remote=NAME
The name the *remote* instance to monitor. If left unspecified, a
connection is made to the default PipeWire instance.

\par -h | \--help
Show help.

\par \--version
Show version information.

# LISTING PORTS AND LINKS

Specify one of -o, -i or -l to list the matching optional input and
output ports and their links.

\par -o | \--output
List output ports

\par -i | \--input
List input ports

\par -l | \--links
List links

\par -m | \--monitor
Monitor links and ports. **pw-link** will not exit but monitor and print
new and destroyed ports or links.

\par -I | \--id
List IDs. Also list the unique link and port ids.

\par -v | \--verbose
Verbose port properties. Also list the port-object-path and the
port-alias.

# CONNECTING PORTS

Without any list option (-i, -o or -l), the given ports will be linked.
Valid port specifications are:

*port-id*

As obtained with the -I option when listing ports.

*node-name:port-name*

As obtained when listing ports.

*port-object-path*

As obtained from the first alternative name for the port when listing
them with the -v option.

*port-alias*

As obtained from the second alternative name for the ports when listing
them with the -v option.

Extra options when linking can be given:

\par -L | \--linger
Linger. Will create a link that exists after **pw-link** is destroyed.
This is the default behaviour, unless the -m option is given.

\par -P | \--passive
Passive link. A passive link will keep both nodes it links inactive
unless another non-passive link is activating the nodes. You can use
this to link a sink to a filter and have them both suspended when
nothing else is linked to either of them.

\par -p | \--props=PROPS
Properties as JSON object. Give extra properties when creaing the link.

# DISCONNECTING PORTS

When the -d option is given, an existing link between port is destroyed.

To disconnect port, a single *link-id*, as obtained when listing links
with the -I option, or two port specifications can be given. See the
connecting ports section for valid port specifications.

\par -d | \--disconnect
Disconnect ports

# EXAMPLES

**pw-link** -iol

List all port and their links.

**pw-link** -lm

List all links and monitor changes until **pw-link** is stopped.

**pw-link** paplay:output_FL alsa_output.pci-0000_00_1b.0.analog-stereo:playback_FL

Link the given output port to the input port.

**pw-link** -lI

List links and their Id.

**pw-link** -d 89

Destroy the link with id 89.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-cli_1 "pw-cli(1)"
