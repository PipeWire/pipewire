#!/bin/bash

set -e
cd /build
#meson . build -docs=true -gstreamer=true -systemmd=true
meson . build
cd build
ninja
