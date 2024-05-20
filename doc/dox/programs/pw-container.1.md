\page page_man_pw-container_1 pw-container

The PipeWire container utility

# SYNOPSIS

**pw-container** \[*options*\] \[*PROGRAM*\]

# DESCRIPTION

Run a program in a new security context [1].

**pw-container** will create a new temporary unix socket and uses the
SecurityContext extension API to create a server on this socket with
the given properties. Clients created from this server socket will have
the security properties attached to them.

This can be used to simulate the behaviour of Flatpak or other containers.

Without any arguments, **pw-container** simply creates the new socket
and prints the address on stdout. Other PipeWire programs can then be run
with `PIPEWIRE_REMOTE=<socket-address>` to connect through this security
context.

When *PROGRAM* is given, the `PIPEWIRE_REMOTE` env variable will be set
and *PROGRAM* will be passed to system(). Argument to *PROGRAM* need to be
properly quoted.

# OPTIONS

\par -P | \--properties=VALUE
Set extra context properties as a JSON object.

\par -r | \--remote=NAME
The name the *remote* instance to connect to. If left unspecified, a
connection is made to the default PipeWire instance.

\par -h | \--help
Show help.

\par \--version
Show version information.

# EXIT STATUS

If the security context was successfully created, **pw-container** does
not exit until terminated with a signal. It exits with status 0 if terminated by
SIGINT or SIGTERM in this case.

Otherwise, it exits with nonzero exit status.

# EXAMPLES

**pw-container** 'pw-dump i 0'

Run pw-dump of the Core object. Note the difference in the object permissions
when running pw-dump with and without **pw-container**.

**pw-container** 'pw-dump pw-dump'

Run pw-dump of itself. Note the difference in the Client security tokens when
running pw-dump with and without **pw-container**.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

[1] https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/staging/security-context/security-context-v1.xml - Creating a security context
