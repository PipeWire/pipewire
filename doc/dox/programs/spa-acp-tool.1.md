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

\par card ID | c ID
Probe card

\par info | i
List card info

\par list | l
List all objects

\par list-verbose | lv
List all data

\par list-profiles [ID] | lpr [ID]
List profiles

\par set-profile ID | spr ID
Activate a profile

\par list-ports [ID] | lp [ID]
List ports

\par set-port ID | sp ID
Activate a port

\par list-devices [ID] | ld [ID]
List available devices

\par get-volume ID | gv ID
Get volume from device

\par set-volume ID VOL | v ID VOL
Set volume on device

\par inc-volume ID | v+ ID
Increase volume on device

\par dec-volume ID | v- ID
Decrease volume on device

\par get-mute ID | gm ID
Get mute state from device

\par set-mute ID VAL | sm ID VAL
Set mute on device

\par toggle-mute ID  | m ID
Toggle mute on device

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)"
