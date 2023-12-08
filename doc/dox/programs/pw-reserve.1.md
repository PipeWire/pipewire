\page page_man_pw-reserve_1 pw-reserve

The PipeWire device reservation utility

# SYNOPSIS

**pw-reserve** \[*options*\]

# DESCRIPTION

Reserves a device using the DBus `org.freedesktop.ReserveDevice1`
device reservation scheme [1], waiting until terminated by `SIGINT` or
another signal.

It can also request other applications to release a device. This can
be used to make audio servers such as PipeWire, Pulseaudio, JACK, or
other applications that respect the device reservation protocol, to
ignore a device, or to release a sound device they are already using
so that it can be used by other applications.

# OPTIONS

\par -r | \--release
Request any client currently holding the device to release it, and try
to reserve it after that. If this option is not given and the device
is already in use, **pw-reserve** will exit with error status.

\par -n NAME | \--name=NAME
\parblock
Name of the device to reserve. By convention, this is

- Audio<em>N</em>: for ALSA card number <em>N</em>

**pw-reserve** can reserve any device name, however PipeWire does
not currently support other values than listed above.
\endparblock

\par -a NAME | \--appname=NAME
Application name to use when reserving the device.

\par -p PRIO | \--priority=PRIO
Priority to use when reserving the device.

\par -m | \--monitor
Monitor reservations of a given device, instead of reserving it.

\par -h | \--help
Show help.

\par \--version
Show version information.

# EXIT STATUS

If the device reservation succeeds, **pw-reserve** does not exit until
terminated with a signal. It exits with status 0 if terminated by
SIGINT or SIGTERM in this case.

Otherwise, it exits with nonzero exit status.

# EXAMPLES

**pw-reserve** -n Audio0

Reserve ALSA card 0, and exit with error if it is already reserved.

**pw-reserve** -n Audio0 -r

Reserve ALSA card 0, requesting any applications that have reserved
the device to release it for us.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

[1] https://git.0pointer.net/reserve.git/tree/reserve.txt - A simple device reservation scheme with DBus
