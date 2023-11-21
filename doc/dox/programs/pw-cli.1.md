\page page_man_pw-cli_1 pw-cli

The PipeWire Command Line Interface

# SYNOPSIS

**pw-cli** \[*command*\]

# DESCRIPTION

Interact with a PipeWire instance.

When a command is given, **pw-cli** will execute the command and exit

When no command is given, **pw-cli** starts an interactive session with
the default PipeWire instance *pipewire-0*.

Connections to other, remote instances can be made. The current instance
name is displayed at the prompt.

Note that **pw-cli** also creates a local PipeWire instance. Some
commands operate on the current (remote) instance and some on the local
instance, such as module loading.

Use the 'help' command to list the available commands.

# GENERAL COMMANDS

\par help | h
Show a quick help on the commands available. It also lists the aliases
for many commands.

\par quit | q
Exit from **pw-cli**

# MODULE MANAGEMENT

Modules are loaded and unloaded in the local instance, thus the pw-cli
binary itself and can add functionality or objects to the local
instance. It is not possible in PipeWire to load modules in another
instance.

\par load-module *name* \[*arguments...*\]
\parblock
Load a module specified by its name and arguments in the local instance.
For most modules it is OK to be loaded more than once.

This command returns a module variable that can be used to unload the
module.

The locally module is *not* visible in the remote instance. It is not
possible in PipeWire to load modules in a remote instance.
\endparblock

\par unload-module *module-var*
Unload a module, specified either by its variable.

# OBJECT INTROSPECTION

\par list-objects
List the objects of the current instance.

Objects are listed with their *id*, *type* and *version*.

\par info *id* | *all*
Get information about a specific object or *all* objects.

Requesting info about an object will also notify you of changes.

# WORKING WITH REMOTES

\par connect \[*remote-name*\]
\parblock
Connect to a remote instance and make this the new current instance.

If no remote name is specified, a connection is made to the default
remote instance, usually *pipewire-0*.

The special remote name called *internal* can be used to connect to the
local **pw-cli** PipeWire instance.

This command returns a remote var that can be used to disconnect or
switch remotes.
\endparblock

\par disconnect \[*remote-var*\]
\parblock
Disconnect from a *remote instance*.

If no remote name is specified, the current instance is disconnected.
\endparblock

\par list-remotes
List all *remote instances*.

\par switch-remote \[*remote-var*\]
\parblock
Make the specified *remote* the current instance.

If no remote name is specified, the first instance is made current.
\endparblock

# NODE MANAGEMENT

\par create-node *factory-name* \[*properties...*\]
\parblock
Create a node from a factory in the current instance.

Properties are key=value pairs separated by whitespace.

This command returns a *node variable*.
\endparblock

\par export-node *node-id* \[*remote-var*\]
Export a node from the local instance to the specified instance. When no
instance is specified, the node will be exported to the current
instance.

# DEVICE MANAGEMENT

\par create-device *factory-name* \[*properties...*\]
\parblock
Create a device from a factory in the current instance.

Properties are key=value pairs separated by whitespace.

This command returns a *device variable*.
\endparblock

# LINK MANAGEMENT

\par create-link *node-id* *port-id* *node-id* *port-id* \[*properties...*\]
\parblock
Create a link between 2 nodes and ports.

Port *ids* can be *-1* to automatically select an available port.

Properties are key=value pairs separated by whitespace.

This command returns a *link variable*.
\endparblock

# GLOBALS MANAGEMENT

\par destroy *object-id*
Destroy a global object.

# PARAMETER MANAGEMENT

\par enum-params *object-id* *param-id*
\parblock
Enumerate params of an object.

*param-id* can also be given as the param short name.
\endparblock

\par set-param *object-id* *param-id* *param-json*
\parblock
Set param of an object.

*param-id* can also be given as the param short name.
\endparblock

# PERMISSION MANAGEMENT

\par permissions *client-id* *object-id* *permission*
\parblock
Set permissions for a client.

*object-id* can be *-1* to set the default permissions.
\endparblock

\par get-permissions *client-id*
Get permissions of a client.

# COMMAND MANAGEMENT

\par send-command *object-id*
Send a command to an object.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-mon_1 "pw-mon(1)",
