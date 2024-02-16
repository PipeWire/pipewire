\page page_man_pw-jack_1 pw-jack

Use PipeWire instead of JACK

# SYNOPSIS

**pw-jack** \[*options*\] *COMMAND* \[*ARGUMENTS...*\]

# DESCRIPTION

**pw-jack** modifies the `LD_LIBRARY_PATH` environment variable so that
applications will load PipeWire's reimplementation of the JACK client
libraries instead of JACK's own libraries. This results in JACK clients
being redirected to PipeWire.

If PipeWire's reimplementation of the JACK client libraries has been
installed as a system-wide replacement for JACK's own libraries, then
the whole system already behaves in that way, in which case **pw-jack**
has no practical effect.

# OPTIONS

\par -h
Show help.

\par -r NAME
The name of the remote instance to connect to. If left unspecified, a
connection is made to the default PipeWire instance.

\par -v
Verbose operation.

# EXAMPLES

\par pw-jack sndfile-jackplay /usr/share/sounds/freedesktop/stereo/bell.oga

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
**jackd(1)**,
