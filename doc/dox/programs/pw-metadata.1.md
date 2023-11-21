\page page_man_pw-metadata_1 pw-metadata

The PipeWire metadata

# SYNOPSIS

**pw-metadata** \[*options*\] \[*id* \[*key* \[*value* \[*type* \] \] \] \]

# DESCRIPTION

Monitor, set and delete metadata on PipeWire objects.

Metadata are key/type/value triplets attached to objects identified by
*id*. The metadata is shared between all applications binding to the
same metadata object. When an object is destroyed, all its metadata is
automatically removed.

When no *value* is given, **pw-metadata** will query and log the
metadata matching the optional arguments *id* and *key*. Without any
arguments, all metadata is displayed.

When *value* is given, **pw-metadata** will set the metadata for *id*
and *key* to *value* and an optional *type*.

# OPTIONS

\par -r | \--remote=NAME
The name the remote instance to use. If left unspecified, a connection
is made to the default PipeWire instance.

\par -h | \--help
Show help.

\par \--version
Show version information.

\par -l | \--list
List available metadata objects

\par -m | \--monitor
Keeps running and log the changes to the metadata.

\par -d | \--delete
Delete all metadata for *id* or for the specified *key* of object *id*.
Without any option, all metadata is removed.

\par -n | \--name
Metadata name (Default: "default").

# EXAMPLES

**pw-metadata**

Show metadata in default name.

**pw-metadata** -n settings 0

Display settings.

**pw-metadata** -n settings 0 clock.quantum 1024

Change clock.quantum to 1024.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-mon_1 "pw-mon(1)",
\ref page_man_pw-cli_1 "pw-cli(1)",
