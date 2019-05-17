#!/bin/sh

# This file is part of PipeWire
#
# PipeWire is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# PipeWire is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with PipeWire; if not, see <http://www.gnu.org/licenses/>.

# Only there to make jhbuild happy

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
