#!/usr/bin/env python3
# -*- mode: python; coding: utf-8; eval: (blacken-mode); -*-
r"""
Doxygen input filter that:

- adds \privatesection to all files
- removes macros
- parses module_args valid_args[] and substitutes it into @pulse_module_options@

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
        type_names = {
            "MODULE_TYPE_STRING": "string",
            "MODULE_TYPE_STRINGV": "stringv",
            "MODULE_TYPE_STRINGE": "stringe",
            "MODULE_TYPE_INT": "int",
            "MODULE_TYPE_BOOL": "bool",
            "MODULE_TYPE_USEC": "usec",
            "MODULE_TYPE_MSEC": "msec",
            "MODULE_TYPE_PROPS": "proplist",
            "MODULE_TYPE_FORMAT": "format",
            "MODULE_TYPE_CHMAP": "chmap",
        }
        res = []
        args_match = re.search(
            r'static const struct module_args valid_args\[\]\s*=\s*\{(.*?)\{\s*NULL',
            text,
            re.S,
        )
        args_text = args_match.group(1) if args_match else ""
        for m in re.finditer(
            r'{\s*"([a-z0-9_]+)"\s*,\s*"([^"]*)"\s*,\s*([^,]*),\s*([^,]*),\s*([^,}]*)',
            args_text,
        ):
            name = m.group(1)
            desc = m.group(2)
            flags = m.group(3).strip()
            typ = m.group(4).strip()
            defval = m.group(5).strip().strip('"')
            parts = []
            typ = type_names.get(typ, "")
            if typ:
                parts.append(typ)
            if defval and defval != "NULL":
                parts.append(f"default {defval}")
            parts.append(desc)
            entry = f"- `{name}`: {', '.join(parts)}"
            if "MODULE_ARG_MANDATORY" in flags:
                entry += " *(mandatory)*"
            if "MODULE_ARG_ENOTIMPL" in flags:
                entry += " *(not implemented)*"
            res.append(entry)

        res = "\n * ".join(res)
        text = text.replace("@pulse_module_options@", res)

    if os.path.basename(fn).startswith("module-") and fn.endswith(".c"):
        text = re.sub(r"^ \* ##", r" * #", text, flags=re.M)

    print("/** \\privatesection */")
    print(text)


if __name__ == "__main__":
    main()
