#!/bin/sh

set -e

while getopts ":b:" opt; do
	case ${opt} in
		b)
			BUILDDIR=${OPTARG}
			;;
		\?)
			echo "Invalid option: -${OPTARG}"
			exit -1
			;;
		:)
			echo "Option -${OPTARG} requires an argument"
			exit -1
			;;
	esac
done

if [ -z "${BUILDDIR}" ]; then
	BUILDDIR=${PWD}/build
	echo "Using default build directory: ${BUILDDIR}"
fi

if [ ! -d ${BUILDDIR} ]; then
	echo "Invalid build directory: ${BUILDDIR}"
	exit -1
fi

export PIPEWIRE_CONFIG_FILE="${BUILDDIR}/src/daemon/pipewire.conf"
export SPA_PLUGIN_DIR="${BUILDDIR}/spa/plugins"
export PIPEWIRE_MODULE_DIR="${BUILDDIR}/src/modules"
export PATH="${BUILDDIR}/src/daemon:${BUILDDIR}/src/tools:${BUILDDIR}/src/examples:${PATH}"
export LD_LIBRARY_PATH="${BUILDDIR}/pipewire-pulseaudio/src/:${BUILDDIR}/src/pipewire/:${BUILDDIR}/pipewire-jack/src/:${LD_LIBRARY_PATH}"
export GST_PLUGIN_PATH="${BUILDDIR}/src/gst/:${GST_PLUGIN_PATH}"

# FIXME: find a nice, shell-neutral way to specify a prompt
${SHELL}
