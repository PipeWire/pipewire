#!/usr/bin/python3
# -*- mode: python; coding: utf-8; eval: (blacken-mode); -*-
r"""
Fetch right Doxygen man file, replace dummy parts, and fixup nroff
"""
import argparse
import re
import sys
from subprocess import call
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__.strip())
    p.add_argument("htmldir", type=Path)
    p.add_argument("page")
    p.add_argument("name")
    p.add_argument("section")
    p.add_argument("version")
    args = p.parse_args()

    page, name, section, version = args.page, args.name, args.section, args.version

    mandir = args.htmldir / ".." / "man" / "man3"
    fn = mandir / f"{page}.3"

    # Doxygen < 1.9.7 names .md file output differently...
    if not fn.exists():
        page2 = page.replace("page_man_", "md_doc_dox_programs_").replace("-", "_")
        fn = mandir / f"{page2}.3"
    else:
        page2 = None

    try:
        with open(fn, "r") as f:
            text = f.read()
    except:
        print(f"ERROR: man file {fn} missing!", file=sys.stderr)
        call(["ls", "-R", str(args.htmldir / ".." / "man")], stdout=sys.stderr)
        raise

    text = text.replace(page, name)
    if page2 is not None:
        text = text.replace(page2, name)

    # Replace bad nroff header
    text = re.sub(
        r"^(\.TH[^\n]*)\n",
        rf'.TH "{name}" {section} "{version}" "PipeWire" \\" -*- nroff -*-\n',
        text,
    )

    # Fixup name field (can't be done in Doxygen, otherwise HTML looks bac)
    text = re.sub(
        rf"^\.SH NAME\s*\n{name} \\- {name}\s*\n\.PP\n *",
        rf".SH NAME\n{name} \\- ",
        text,
        count=1,
        flags=re.M,
    )

    # Add DESCRIPTION section if missing and NAME field has extra stuff
    if not re.search(r"^\.SH DESCRIPTION\s*\n", text):
        text = re.sub(
            r"^(.SH NAME\s*\n[^\.].*\n)\.PP\s*\n([^\.\n ]+)",
            r"\1.SH DESCRIPTION\n.PP\n\2",
            text,
            count=1,
            flags=re.M,
        )

    # Upcase titles
    def upcase(m):
        return m.group(0).upper()

    text = re.sub(r"^\.SH .*?$", upcase, text, flags=re.M)

    # Replace PW_KEY_*, SPA_KEY_* by their values
    def pw_key(m):
        key = m.group(0)
        key = key.replace("PW_KEY_", "").lower().replace("_", ".")
        if key in ("protocol", "access", "client.access") or key.startswith("sec."):
            return f"pipewire.{key}"
        return key

    def spa_key(m):
        key = m.group(0)
        return key.replace("SPA_KEY_", "").lower().replace("_", ".")

    text = re.sub(r"PW_KEY_[A-Z_]+", pw_key, text, flags=re.S)
    text = re.sub(r"SPA_KEY_[A-Z_]+", spa_key, text, flags=re.S)

    print(text)


if __name__ == "__main__":
    main()
