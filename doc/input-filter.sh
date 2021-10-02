#!/bin/bash
#
# Doxygen input filter that adds \privatesection to all files,
# and removes macros.
#
# This is used for .c files, and causes Doxygen to not include
# any symbols from them, unless they also appeared in a header file.
#
echo -n "/** \privatesection */ "
sed -e 's/#define.*//' < "$1"
