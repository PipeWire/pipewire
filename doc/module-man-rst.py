#!/usr/bin/python3
"""
Convert Doxygen HTML documentation for a PipeWire module to RST.
"""
import argparse
import html, html.parser
import re
from pathlib import Path
from subprocess import check_output

TEMPLATE = """
{name}
{name_underline}

{subtitle_underline}
{subtitle}
{subtitle_underline}

:Manual section: 7
:Manual group: PipeWire

DESCRIPTION
-----------

{content}

AUTHORS
-------

The PipeWire Developers <{PACKAGE_BUGREPORT}>;
PipeWire is available from {PACKAGE_URL}

SEE ALSO
--------

``pipewire(1)``,
``pipewire.conf(5)``,
``libpipewire-modules(7)``
"""


def main():
    p = argparse.ArgumentParser(description=__doc__.strip())
    p.add_argument(
        "-D",
        "--define",
        nargs=2,
        action="append",
        dest="define",
        default=[],
    )
    p.add_argument("pandoc")
    p.add_argument("module")
    p.add_argument("htmldir", type=Path)
    args = p.parse_args()

    page = args.module.lower().replace("-", "_")
    src = args.htmldir / f"page_{page}.html"

    # Pick content block only
    parser = DoxyParser()
    with open(src, "r") as f:
        parser.feed(f.read())
    data = "".join(parser.content)

    # Produce output
    content = check_output(
        [args.pandoc, "-f", "html", "-t", "rst"], input=data, encoding="utf-8"
    )

    if not content.strip():
        content = "Undocumented."

    name = f"libpipewire-{args.module}"
    subtitle = "PipeWire module"

    env = dict(
        content=content,
        name=name,
        name_underline="#" * len(name),
        subtitle=subtitle,
        subtitle_underline="-" * len(subtitle),
    )

    for k, v in args.define:
        env[k] = v

    print(TEMPLATE.format(**env))


def replace_pw_key(key):
    key = key.lower().replace("_", ".")
    if key in ("protocol", "access", "client.access") or key.startswith("sec."):
        return f"pipewire.{key}"
    return key


class DoxyParser(html.parser.HTMLParser):
    """
    Capture div.textblock, and:
    - Convert div.fragment to pre
    - Convert a[@href="page_module_XXX.html"] to <tt>libpipewire-module-xxx(7)</tt>
    """

    def __init__(self):
        super().__init__()
        self.content = []
        self.stack = []

    def feed(self, data):
        try:
            super().feed(data)
        except EOFError:
            pass

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)

        if self.stack:
            if self.stack[-1] is None:
                self.stack.append(None)
                return

            if tag == "div" and attrs.get("class") == "fragment":
                tag = "pre"
                attrs = dict()
            elif tag == "a" and attrs.get("href").startswith("page_module_"):
                module = attrs["href"].replace("page_module_", "libpipewire-module-")
                module = module.replace(".html", "").replace("_", "-")
                self.content.append(f"<tt>{module}(7)</tt>")
                self.stack.append(None)
                return

            attrstr = " ".join(f'{k}="{html.escape(v)}"' for k, v in attrs.items())
            self.content.append(f"<{tag} {attrstr}>")
            self.stack.append(tag)
        elif tag == "div" and attrs.get("class") == "textblock":
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if len(self.stack) == 1:
            raise EOFError()
        elif self.stack:
            otag = self.stack.pop()
            if otag is not None:
                self.content.append(f"</{otag}>")

    def handle_data(self, data):
        if self.stack and self.stack[-1] is not None:
            if self.stack[-1] == "a":
                m = re.match(r"^(PW|SPA)_KEY_([A-Z_]+)$", data)
                if m:
                    data = replace_pw_key(m.group(2))

            self.content.append(html.escape(data))


if __name__ == "__main__":
    main()
