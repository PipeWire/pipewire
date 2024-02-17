#!/usr/bin/env python3
# -*- mode: python; coding: utf-8; eval: (blacken-mode); -*-
r"""input-filter-md.py FILENAME
input-filter-md.py --index FILENAMES...

Doxygen .md input filter that adds extended syntax.
With --index, generates an index file.
Assumes BUILD_DIR environment variable is set.

@PAR@ <section> <name> (...)

    Adds an index item and expands to

    \anchor <key>
    \par <name> (...)

@SECREF@ <section>

    Expands to

    \secreflist
    \refitem ...
    ...
    \endsecreflist

    containing all index items from the specified section.

# Section title    @IDX@ <section>

   Adds the section title to the index, and expands to an anchor

   # Section title {#key}

The index keys can be used in \ref and have format

    {section}__{name}

where the parts are converted to lowercase and _ replaces
non-alphanumerics.

"""
import sys
import re
import os


def index_key(section, name):
    key = f"{section}__{name}".lower()
    return re.sub(r"[^A-Za-z0-9_-]", "_", key)


BUILD_DIR = os.environ["BUILD_DIR"]
PAR_RE = r"^@PAR@\s+([^\s]*)[ \t]+(\S+)(.*)$"
IDX_RE = r"^(#+)(.*)@IDX@[ \t]+(\S+)[ \t]*$"
SECREF_RE = r"^@SECREF@[ \t]+([^\n]*)[ \t]*$"


def main(args):
    fn = args[0]
    with open(fn, "r") as f:
        text = f.read()

    def par(m):
        section = m.group(1)
        name = m.group(2)
        rest = m.group(3).strip()
        key = index_key(section, name)
        return f"\\anchor {key}\n\\par {name} {rest}"

    def idx(m):
        level = m.group(1)
        title = name = m.group(2).strip()
        section = m.group(3)
        if title == title.upper():
            name = name.capitalize()
        key = index_key(section, name)
        return f"{level} {title} {{#{key}}}"

    def secref(m):
        import os
        import json

        secs = m.group(1).split()

        with open(os.path.join(BUILD_DIR, "index.json"), "r") as f:
            index = json.load(f)

        items = {}

        for sec in secs:
            if sec not in index:
                print(f"{fn}: no index '{sec}'", file=sys.stderr)
            else:
                for name, key in index[sec].items():
                    if name in items:
                        pkey, psec = items.pop(name)
                        nname = f"{name} ({sec})"
                        items[nname] = (key, sec)
                        if pkey is not None:
                            pname = f"{name} ({psec})"
                            items[pname] = (pkey, psec)
                            items[name] = (None, None)
                    else:
                        items[name] = (key, sec)

        text = [r"\secreflist"]
        for name, (key, sec) in sorted(items.items()):
            if key is not None:
                text.append(rf'\refitem {key} "{name}"')
        text.append(r"\endsecreflist")
        text = "\n".join(text)
        return f"{text}\n"

    text = re.sub(PAR_RE, par, text, flags=re.M)
    text = re.sub(IDX_RE, idx, text, flags=re.M)
    text = re.sub(SECREF_RE, secref, text, flags=re.M)

    print(text)


def main_index(args):
    import json

    sections = {}

    for fn in set(args):
        with open(fn, "r") as f:
            load_index(sections, f.read())

    result = {}

    for section, items in sections.items():
        for name in items:
            key = index_key(section, name)
            result.setdefault(section, {})[name] = key

    with open(os.path.join(BUILD_DIR, "index.json"), "w") as f:
        json.dump(result, f)


def load_index(sections, text):
    def par(m):
        section = m.group(1)
        name = m.group(2)
        sections.setdefault(section, []).append(name)
        return ""

    def idx(m):
        name = m.group(2).strip()
        section = m.group(3)
        if name == name.upper():
            name = name.capitalize()
        sections.setdefault(section, []).append(name)
        return ""

    text = re.sub(PAR_RE, par, text, flags=re.M)
    text = re.sub(IDX_RE, idx, text, flags=re.M)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--index":
        main_index(sys.argv[2:])
    elif len(sys.argv) == 2:
        main(sys.argv[1:])
    else:
        print(__doc__.strip())
        sys.exit(1)
