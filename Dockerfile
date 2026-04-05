# Build environment for PipeWire
#
# To build the image and tag it with “pw”:
# docker build . -t pw
#
# To run the container with the current directory mounted (so you can build inside the container):
# docker run -it --rm -v $(pwd):/repo pw
#
# In the container, to build docs and not too much else, set up the build directory in “builddir”:
# meson setup builddir --reconfigure --auto-features=disabled -Ddocs=enabled -Dsession-managers=[] -Dflatpak=disabled -Ddbus=disabled -Dpipewire-alsa=disabled
#
# Once configured, build the configured components:
# cd builddir
# meson compile


FROM ubuntu:24.04

RUN DEBIAN_FRONTEND=noninteractive apt-get update \
    && apt-get -y install \
    debhelper-compat \
    findutils \
    git \
    libapparmor-dev \
    libasound2-dev \
    libavcodec-dev \
    libavfilter-dev \
    libavformat-dev \
    libdbus-1-dev \
    libbluetooth-dev \
    libglib2.0-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libsbc-dev \
    libsdl2-dev \
    libsnapd-glib-dev \
    libudev-dev \
    libva-dev \
    libx11-dev \
    meson \
    ninja-build \
    pkg-config \
    python3-docutils \
    systemd

RUN DEBIAN_FRONTEND=noninteractive apt-get -y install \
    doxygen

USER ubuntu
WORKDIR /repo

