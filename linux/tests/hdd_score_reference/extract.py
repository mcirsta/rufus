# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the current Windows wrapper with test-only platform facts."""
from pathlib import Path
import json
import sys


source = Path(sys.argv[1])
text = source.read_text(encoding="utf-8")
signature = "int IsHDD(DWORD DriveIndex, uint16_t vid, uint16_t pid, const char* strid)"
if text.count(signature) != 1:
    raise SystemExit("IsHDD signature changed; review the reference harness")
start = text.index(signature)
body = text[start:]
# IsHDD currently ends smart.c. Refuse a changed extraction boundary instead of
# silently compiling a partial function or unrelated production entry points.
if body.count("{") != body.count("}") or not body.rstrip().endswith("return result.total;\n}"):
    raise SystemExit("IsHDD boundary changed; review the reference harness")
license_text = text[:text.index("#ifdef _CRTDBG_MAP_ALLOC")]
line = text.count("\n", 0, start) + 1
Path(sys.argv[2]).write_text(license_text + f"#line {line} {json.dumps(str(source))}\n" + body,
                           encoding="utf-8")
