# PipeWire

PipeWire is a server and user space API to deal with multimedia
pipelines. This includes:

  - Making available sources of video (such as from a capture devices or
    application provided streams) and multiplexing this with
    clients.
  - Accessing sources of video for consumption.
  - Generating graphs for audio and video processing.

Nodes in the graph can be implemented as separate processes,
communicating with sockets and exchanging multimedia content using fd
passing.

## Building

Pipewire uses the Meson and Ninja build system to compile. You can run it
with:

```
$ meson build
$ cd build
$ ninja
```

You can see the available meson options in `meson_options.txt` file.

If you're not familiar with these tools, the included `autogen.sh` script will
automatically run the correct `meson`/`ninja` commands, and output a Makefile.
It follows that there are two methods to build Pipewire, however both rely
on Meson and Ninja to actually perform the compilation:

```
$ ./autogen.sh
$ make
```

## Running

If you want to run PipeWire without installing it on your system, there is a
script that you can run. This puts you in an environment in which PipeWire can
be run from the build directory, and ALSA, PulseAudio and JACK applications
will use the PipeWire emulation libraries automatically
in this environment. You can get into this environment with:

```
$ ./pw-uninstalled.sh
```
