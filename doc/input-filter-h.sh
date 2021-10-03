#!/bin/bash
#
# Doxygen input filter, which tries to fix documentation of callback
# method macros.
#
# This is used for .h files.
#

FILENAME="$1"

# Add \ingroup commands for the file, for each \addgroup in it
BASEFILE=$(echo "$FILENAME" | sed -e 's@.*src/pipewire/@pipewire/@; s@.*spa/include/spa/@spa/@; s@.*src/test/@test/@;')

echo "/** \file"
echo "\`$BASEFILE\`"
sed -n -e '/.*\\addtogroup [a-zA-Z0-9_].*/ { s/.*addtogroup /\\ingroup /; p; }' < "$FILENAME"
echo " */"

# Add \sa and \copydoc for (struct *methods) callback macros.
# #define pw_core_add_listener(...) pw_core_method(c,add_listener,...) -> add \sa and \copydoc
# #define spa_system_read(...) spa_system_method_r(c,read,...) -> add \sa and \copydoc
sed -e 's@^\(#define .*[[:space:]]\)\(.*_method\)\((.,[[:space:]]*\)\([a-z_]\+\)\(.*)[[:space:]]*\)$@\1\2\3\4\5 /**< \\copydoc \2s.\4\n\n\\sa \2s.\4 */@;' \
    -e 's@^\(#define .*[[:space:]]\)\(.*_method\)\(_[rvs](.,[[:space:]]*\)\([a-z_]\+\)\(.*)[[:space:]]*\)$@\1\2\3\4\5 /**< \\copydoc \2s.\4\n\n\\sa \2s.\4 */@;' \
< "$FILENAME"
