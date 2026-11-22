#!/usr/bin/env python3
"""Generate the Android settings form with scroll contents and pages already parented."""

import re
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree


def main(uic, form, output):
    form = Path(form)
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        generated = Path(tmp) / output.name
        subprocess.run([uic, str(form), "-o", str(generated)], check=True)
        source = generated.read_text(encoding="utf-8")

    # uic normally builds each subtree before adding it to its container.
    # Start with the same parent to avoid restyling all descendants at insertion.
    for widget in ElementTree.parse(form).iter("widget"):
        container = widget.get("class")
        if container not in ("QScrollArea", "QStackedWidget"):
            continue
        parent = widget.get("name")
        if container == "QScrollArea":
            parent += "->viewport()"
        for page in widget.findall("widget"):
            if page.get("class") != "QWidget":
                continue
            name = page.get("name")
            constructor = re.compile(rf"^( +{re.escape(name)} = new QWidget\()\);$", re.MULTILINE)
            source, count = constructor.subn(lambda match: f"{match[1]}{parent});", source)
            if count != 1:
                raise RuntimeError(f"Unexpected uic constructor for settings widget {name}")

    output.write_text(source, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main(*sys.argv[1:])
