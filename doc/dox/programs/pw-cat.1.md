\page page_man_pw-cat_1 pw-cat

Play and record media with PipeWire

# SYNOPSIS

**pw-cat** \[*options*\] \[*FILE* \| -\]

**pw-play** \[*options*\] \[*FILE* \| -\]

**pw-record** \[*options*\] \[*FILE* \| -\]

**pw-midiplay** \[*options*\] \[*FILE* \| -\]

**pw-midirecord** \[*options*\] \[*FILE* \| -\]

**pw-midi2play** \[*options*\] \[*FILE* \| -\]

**pw-midi2record** \[*options*\] \[*FILE* \| -\]

**pw-dsdplay** \[*options*\] \[*FILE* \| -\]

# DESCRIPTION

**pw-cat** is a simple tool for playing back or capturing raw or encoded
media files on a PipeWire server. It understands all audio file formats
supported by `libsndfile` for PCM capture and playback. When capturing
PCM, the filename extension is used to guess the file format with the
WAV file format as the default.

It understands standard MIDI files and MIDI 2.0 clip files for playback
and recording. This tool will not render MIDI files, it will simply make
the MIDI events available to the graph. You need a MIDI renderer such as
qsynth, timidity or a hardware MIDI renderer to hear the MIDI.

DSD playback is supported with the DSF file format. This tool will only
work with native DSD capable hardware and will produce an error when no
such hardware was found.

When the *FILE* is - input and output will be raw data from STDIN and
STDOUT respectively.

# OPTIONS

\par -h | \--help
Show help.

\par \--version
Show version information.

\par -v | \--verbose
Verbose operation.

\par -R | \--remote=NAME
The name the *remote* instance to connect to. If left unspecified, a
connection is made to the default PipeWire instance.

\par -p | \--playback
Playback mode. Read data from the specified file, and play it back. If
the tool is called under the name **pw-play**, **pw-midiplay** or
**pw-midi2play** this is the default.

\par -r | \--record
Recording mode. Capture data and write it to the specified file. If the
tool is called under the name **pw-record**, **pw-midirecord** or
**pw-midi2record** this is the default.

\par -m | \--midi
MIDI mode. *FILE* is a MIDI file. If the tool is called under the name
**pw-midiplay** or **pw-midirecord** this is the default. Note that this
program will *not* render the MIDI events into audible samples, it will
simply provide the MIDI events in the graph. You need a separate MIDI
renderer such as qsynth, timidity or a hardware renderer to hear the
MIDI.

\par -c | \--midi-clip
MIDI 2.0 clip mode. *FILE* is a MIDI 2.0 clip file. If the tool is called
under the name **pw-midi2play** or **pw-midi2record** this is the default.
Note that this program will *not* render the MIDI events into audible
samples, it will simply provide the MIDI events in the graph. You need a
separate MIDI renderer such as qsynth, timidity or a hardware renderer to
hear the MIDI.

\par -d | \--dsd
DSD mode. *FILE* is a DSF file. If the tool is called under the name
**pw-dsdplay** this is the default. Note that this program will *not*
render the DSD audio. You need a DSD capable device to play DSD content
or this program will exit with an error.

\par \--media-type=VALUE
Set the media type property (default Audio/Midi depending on mode). The
media type is used by the session manager to select a suitable target to
link to.

\par \--media-category=VALUE
Set the media category property (default Playback/Capture depending on
mode). The media type is used by the session manager to select a
suitable target to link to.

\par \--media-role=VALUE
Set the media role property (default Music). The media type is used by
the session manager to select a suitable target to link to.

\par \--target=VALUE
\parblock
Set a node target (default auto). The value can be:

- **auto**: Automatically select (Default)

- **0**: Don't try to link this node

- <b>\<id\></b>: The object.serial or the node.name of a target node
\endparblock

\par \--latency=VALUE\[*units*\]
\parblock
Set the node latency (default 100ms)

The latency determines the minimum amount of time it takes for a sample
to travel from application to device (playback) and from device to
application (capture).

The latency determines the size of the buffers that the application will
be able to fill. Lower latency means smaller buffers but higher
overhead. Higher latency means larger buffers and lower overhead.

Units can be **s** for seconds, **ms** for milliseconds, **us** for
microseconds, **ns** for nanoseconds. If no units are given, the latency
value is samples with the samplerate of the file.
\endparblock

\par -P | \--properties=VALUE
Set extra stream properties as a JSON object.

\par -q | \--quality=VALUE
Resampler quality. When the samplerate of the source or destination file
does not match the samplerate of the server, the data will be resampled.
Higher quality uses more CPU. Values between 0 and 15 are allowed, the
default quality is 4.

\par \--rate=VALUE
The sample rate, default 48000.

\par \--channels=VALUE
The number of channels, default 2.

\par \--channel-map=VALUE
The channelmap. Possible values include: **mono**, **stereo**,
**surround-21**, **quad**, **surround-22**, **surround-40**,
**surround-31**, **surround-41**, **surround-50**, **surround-51**,
**surround-51r**, **surround-70**, **surround-71** or a comma separated
list of channel names: **FL**, **FR**, **FC**, **LFE**, **SL**, **SR**,
**FLC**, **FRC**, **RC**, **RL**, **RR**, **TC**, **TFL**, **TFC**,
**TFR**, **TRL**, **TRC**, **TRR**, **RLC**, **RRC**, **FLW**, **FRW**,
**LFE2**, **FLH**, **FCH**, **FRH**, **TFLC**, **TFRC**, **TSL**,
**TSR**, **LLFR**, **RLFE**, **BC**, **BLC**, **BRC**

\par \--format=VALUE
The sample format to use. One of: **u8**, **s8**, **s16** (default),
**s24**, **s32**, **f32**, **f64**.

\par \--volume=VALUE
The stream volume, default 1.000. Depending on the locale you have
configured, "," or "." may be used as a decimal separator. Check with
**locale** command.

# AUTHORS

The PipeWire Developers <$(PACKAGE_BUGREPORT)>;
PipeWire is available from <$(PACKAGE_URL)>

# SEE ALSO

\ref page_man_pipewire_1 "pipewire(1)",
\ref page_man_pw-mon_1 "pw-mon(1)",
