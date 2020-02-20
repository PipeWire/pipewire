#!/bin/sh

# Only there to make jhbuild happy

set -e

git submodule init
git submodule update

if [ -z $MESON ]; then
	MESON=`which meson`
fi
if [ -z $MESON ]; then
	echo "error: Meson not found."
	echo "Install meson to configure and build Pipewire. If meson" \
	     "is already installed, set the environment variable MESON" \
	     "to the binary's path."
	exit 1;
fi

mkdir -p build
$MESON setup "$@" build # use 'autogen.sh --reconfigure' to update
ln -sf build/Makefile Makefile

ln -sf libjack.so.0 build/pipewire-jack/src/libjack.so
ln -sf libjack-pw.so build/pipewire-jack/src/libjack.so.0

ln -sf libpulse.so.0 build/pipewire-pulseaudio/src/libpulse.so
ln -sf libpulse-pw.so build/pipewire-pulseaudio/src/libpulse.so.0
ln -sf libpulse-simple.so.0 build/pipewire-pulseaudio/src/libpulse-simple.so
ln -sf libpulse-simple-pw.so build/pipewire-pulseaudio/src/libpulse-simple.so.0
ln -sf libpulse-mainloop-glib.so.0 build/pipewire-pulseaudio/src/libpulse-mainloop-glib.so
ln -sf libpulse-mainloop-glib-pw.so build/pipewire-pulseaudio/src/libpulse-mainloop-glib.so.0
