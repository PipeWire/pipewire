#!/bin/sh

# Only there to make jhbuild happy

git submodule init
git submodule update

mkdir -p build
meson setup "$@" build  # use 'autogen.sh --reconfigure' to update
ln -sf build/Makefile Makefile
