#!/bin/bash
#
# Replicates the source tree at $SOURCE to $DEST with any static removed
# from the file. This allows Doxygen to parse the file and document static
# inline functions.

SOURCE="$1"
DEST="$2"
test -n "$SOURCE" || (echo "Source argument is missing" && exit 1)
test -n "$DEST" || (echo "Dest argument is missing" && exit 1)

echo "Reading from $SOURCE"
echo "Copying to $DEST"

mkdir -p "$DEST"
cp -rf "$SOURCE"/* "$DEST/"
shopt -s globstar  # proper recursive globbing
sed -i 's|^static|/* \0 */|' "$DEST"/**/*.h
