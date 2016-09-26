#!/bin/sh

# This file is part of Pinos
#
# Pinos is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# Pinos is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with Pinos; if not, see <http://www.gnu.org/licenses/>.

# Only there to make jhbuild happy

rm -rf ./build
mkdir build
meson build $@
