FROM fedora:31
ARG FEDORA_VER=31

LABEL description="Fedora-based environment for building PipeWire" \
      maintainer="George Kiagiadakis <george.kiagiadakis@collabora.com>"

# Install pipewire dependencies
RUN dnf -y install \
    which \
    gcc \
    meson \
    systemd-devel \
    dbus-devel \
    glib-devel \
    gstreamer1-devel \
    gstreamer1-plugins-base-devel \
    jack-audio-connection-kit-devel \
    pulseaudio-libs-devel \
    alsa-lib-devel \
    libv4l-devel \
    libX11-devel \
    SDL2-devel \
    libva-devel \
    bluez-libs-devel \
    sbc-devel \
    doxygen \
    graphviz \
    xmltoman \
    vulkan-loader-devel \
    git \
    make \
    findutils \
    && dnf clean all
