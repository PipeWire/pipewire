#!/bin/sh

# Only there to make jhbuild happy

git submodule init
git submodule update

rm -rf ./build
mkdir build
meson build $@
ln -s build/Makefile Makefile
