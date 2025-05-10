#!/usr/bin/env bash

set -ex

packages=(
	# libapparmor-dev
	libasound2-dev
	libavahi-client-dev
	libavcodec-dev
	libavfilter-dev
	libavformat-dev
	libbluetooth-dev
	libcanberra-dev
	libdbus-1-dev
	libebur128-dev
	libfdk-aac-dev
	libffado-dev
	libfftw3-dev
	libfreeaptx-dev
	libglib2.0-dev
	libgstreamer1.0-dev
	libgstreamer-plugins-base1.0-dev
	libjack-jackd2-dev
	liblc3-dev
	liblilv-dev
	libmysofa-dev
	libopus-dev
	libpulse-dev
	libreadline-dev
	libsbc-dev
	libsdl2-dev
	# libsnapd-glib-dev
	libsndfile1-dev
	libspandsp-dev
	libssl-dev
	libsystemd-dev
	libudev-dev
	libusb-1.0-0-dev
	libvulkan-dev
	libwebrtc-audio-processing-dev
	libx11-dev
	modemmanager-dev
)

arch="$1"

export DEBIAN_FRONTEND=noninteractive

sed -i \
	's/^Components:.*$/Components: main contrib non-free non-free-firmware/' \
	/etc/apt/sources.list.d/debian.sources

dpkg --add-architecture "$arch"
apt update -y

pkgs=( "crossbuild-essential-$arch" )
for pkg in "${packages[@]}"; do
	pkgs+=( "$pkg:$arch" )
done

apt install -y "${pkgs[@]}"

meson env2mfile --cross --debarch "$arch" -o "/opt/meson-$arch.cross"
