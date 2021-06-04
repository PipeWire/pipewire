#!/usr/bin/env python3
#
# Generates a simple .cpp file including all of the SPA headers.
#
# Usage: gen-cpp-test.py path/to/pipewire.git/spa/include/spa

template = """
@@INCLUDES@@

int main(int argc, char *argv[])
{
    return 0;
}
"""

import sys
from pathlib import Path

basedir = Path(sys.argv[1])
includes = [
    "#include <{}>".format(f.relative_to(basedir.parent)) for f in sorted(basedir.rglob("*.h"))
]

print(template.replace("@@INCLUDES@@", "\n".join(includes)))
