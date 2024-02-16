PipeWire's Pulseaudio emulation implements several Pulseaudio modules.
It only supports its own built-in modules, and cannot load external
modules written for Pulseaudio.

# Loading modules

The built-in modules can be loaded using Pulseaudio client programs,
for example `pactl load-module <module-name> <module-options>`. They
can also added to `pipewire-pulse.conf`, typically by a drop-in file
in `~/.config/pipewire/pipewire-pulse.conf.d/` containing the module
name and its arguments
```
pulse.cmd = [
    { cmd = "load-module" args = "module-null-sink sink_name=foo" flags = [ ] }
]
```

To list all modules currently loaded, with their arguments:
```
pactl list modules
```

For a short list of loaded modules:
```
pactl list modules short
```

Modules may be unloaded using either the module-name or index number:

```
pactl load-module <module-name> <parameters>
pactl unload-module <module-name|index#>
```

# Common module options

Most modules that create streams/devices support the following properties:

## sink_name, source_name

Name for the sink (resp. source). Allowed characters in the name are a-z, A-Z, numbers, period (.) and underscore (_). The length must be 1-128 characters.

## format

The sample format. The supported audio formats are:

### PCM
 - u8: unsigned 8-bit integer
 - aLaw: A-law encoded 8-bit integer
 - uLaw: μ-law encoded 8-bit integer
 - s16le: signed 16-bit little-endian integer
 - s16be: signed 16-bit big-endian integer
 - s16, s16ne: native-endian aliases for s16le or s16be
 - s16re: reverse-endian alias for s16le or s16be
 - float32le: 32-bit little-endian float
 - float32be: 32-bit big-endian float
 - float32, float32ne: native-endian aliases for float32le or float32be
 - float32re: reverse-endian alias for float32le or float32be
 - s32le: signed 32-bit little-endian integer
 - s32be: signed 32-bit big-endian integer
 - s32, s32ne: native-endian aliases for s32le or s32be
 - s32re: reverse-endian alias for s32le or s32be
 - s24le: signed 24-bit little-endian integer (note: ALSA calls this "S24_3LE")
 - s24be: signed 24-bit big-endian integer (note: ALSA calls this "S24_3BE")
 - s24, s24ne: native-endian aliases for s24le or s24be
 - s24re: reverse-endian alias for s24le or s24be
 - s24-32le: signed 24-bit little-endian integer, packed into a 32-bit integer so that the 8 most significant bits are ignored (note: ALSA calls this "S24_LE")
 - s24-32be: signed 24-bit big-endian integer, packed into a 32-bit integer so that the 8 most significant bits are ignored (note: ALSA calls this "S24_BE")
 - s24-32, s24-32ne: native-endian aliases for s24-32le or s24-32be
 - s24-32re: reverse-endian alias for s24-32le or s24-32be

### Compressed audio formats

Below is a list of all supported compressed formats. The code at the beginning of each line is used whenever a textual identifier for a format is needed (for example in configuration files or on the command line). The formats whose identifier ends with -iec61937 have to be wrapped in IEC 61937 frames, which makes the compressed audio behave more like normal PCM audio.

 - ac3-iec61937: Dolby Digital (DD / AC-3 / A/52)
 - eac3-iec61937: Dolby Digital Plus (DD+ / E-AC-3)
 - mpeg-iec61937: MPEG-1 or MPEG-2 Part 3 (not MPEG-2 AAC)
 - dts-iec61937: DTS
 - mpeg2-aac-iec61937: MPEG-2 AAC (supported since PulseAudio 4.0)
 - truehd-iec61937: Dolby TrueHD (added in PulseAudio 13.0, but doesn't work yet in practice)
 - dtshd-iec61937: DTS-HD Master Audio (added in PulseAudio 13.0, but doesn't work yet in practice)
 - pcm: PCM (not a compressed format, but listed here, because pcm is one of the recognized encoding identifiers)
 - any: (special identifier for indicating that any encoding can be used)


## rate

The sample rate.

##channels

Number of audio channels.

## channel_map

A channel map. A list of comma-separated channel names. The currently defined channel names are:
`left`, `right`, `mono`, `center`, `front-left`, `front-right`, `front-center`,
`rear-center`, `rear-left`, `rear-right`, `lfe`, `subwoofer`, `front-left-of-center`,
`front-right-of-center`, `side-left`, `side-right`, `aux0`, `aux1` to `aux15`, `top-center`,
`top-front-left`, `top-front-right`, `top-front-center`, `top-rear-left`, `top-rear-right`,
`top-rear-center`

## sink_properties, source_properties

Set additional properties of the sink/source. For example, you can set the description directly
when the module is loaded by setting this parameter.

```
load-module module-alsa-sink sink_name=headphones sink_properties=device.description=Headphones
```
