#!/bin/sh
#
# core-backtrace.sh COREFILE...
#
# Print backtraces from core dump files
#
set -e

for f in "$@"; do
    if [ ! -e "$f" ]; then
        continue
    fi
    echo "#"
    echo "# --- $f ---"
    echo "#"
    exe=$(gdb -q -c "$f" -ex 'info auxv' -ex quit | sed -n -e '/AT_EXECFN/ { s/^[^"]*"//; s/"$//; p; }')
    if [ -f "$exe" ]; then
        gdb -q "$exe" "$f" -ex 'thr a a bt full' -ex quit
    else
        gdb -q -c "$f" -ex 'thr a a bt full' -ex quit
    fi
done
