#!/bin/bash

set -e
meson . build -docs=true -gstreamer=true -systemmd=true
cd build
ninja
