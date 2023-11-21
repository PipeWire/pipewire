#!/usr/bin/env python3
# -*- mode: python; coding: utf-8; eval: (blacken-mode); -*-
r"""
Doxygen input filter that:

- adds \privatesection to all files
- removes macros
- parses pulse_module_options and substitutes it into @pulse_module_options@

This is used for .c files, and causes Doxygen to not include
any symbols from them, unless they also appeared in a header file.

The Pulse module option parsing is used in documentation of Pulseaudio modules.
"""
import sys
import re
import os


def main():
    fn = sys.argv[1]
    with open(fn, "r") as f:
        text = f.read()

    text = re.sub("#define.*", "", text)

    if "@pulse_module_options@" in text:
        m = re.search(
            r"static const char[* ]*const pulse_module_options\s+=\s+(.*?\")\s*;\s*$",
            text,
            re.M | re.S,
        )
        if m:
            res = []
            for line in m.group(1).splitlines():
                m = re.match(r"\s*\"\s*([a-z0-9_]+)\s*=\s*(.*)\"\s*$", line)
                if m:
                    name = m.group(1)
                    value = m.group(2).strip().strip("<>")
                    res.append(f"- `{name}`: {value}")

            res = "\n * ".join(res)
            text = text.replace("@pulse_module_options@", res)

    if os.path.basename(fn).startswith("module-") and fn.endswith(".c"):
        text = re.sub(r"^ \* ##", r" * #", text, flags=re.M)

    print("/** \\privatesection */")
    print(text)


if __name__ == "__main__":
    main()
