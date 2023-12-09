\page page_man_spa-acp-tool_1 spa-acp-tool

The PipeWire ALSA profile debugging utility

# SYNOPSIS

**spa-acp-tool** \[*COMMAND*\]

# DESCRIPTION

Debug tool for exercising the ALSA card profile probing code, without
running PipeWire.

May be used to debug problems where PipeWire has incorrectly
functioning ALSA card profiles.

# COMMANDS

\par help | h
Show available commands

\par quit | q
Quit

\par card <id> | c <id>
Probe card

\par info | i
List card info

\par list | l
List all objects

\par list-verbose | lv
List all data

\par list-profiles [id] | lpr [id]
List profiles

\par set-profile <id> | spr <id>
Activate a profile

\par list-ports [id] | lp [id]
List ports

\par set-port <id> | sp <id>
Activate a port

\par list-devices [id] | ld [id]
List available devices

\par get-volume <id> | gv <id>
Get volume from device

\par set-volume <id> <vol> | v <id> <vol>
Set volume on device

\par inc-volume <id> | v+ <id>
Increase volume on device

\par dec-volume <id> | v- <id>
Decrease volume on device

\par get-mute <id> | gm <id>
Get mute state from device

\par set-mute <id> <val> | sm <id> <val>
Set mute on device

\par toggle-mute <id>  | m <id>
Toggle mute on device

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)"
