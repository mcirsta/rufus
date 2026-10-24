#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inventory literal native Qt translation sources without changing a catalog.

The compiled test_localization inventoryCoverage operation reports actual catalog
bindings for this input. This scanner cannot establish that backend/provider
diagnostics are all translated or that every source is reachable at runtime.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


TOKENS = re.compile(
    r'(?P<comment>//[^\n]*|/\*.*?\*/)|(?P<string>"(?:\\.|[^"\\])*")'
    r'|(?P<identifier>\b[A-Za-z_]\w*\b)|(?P<punct>\S)', re.DOTALL)


def literal_argument(tokens, start):
    parts = []
    while start < len(tokens) and tokens[start].lastgroup == 'string':
        parts.append(ast.literal_eval(tokens[start].group()))
        start += 1
    return ''.join(parts), start


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    messages = {}
    dynamic = []
    files = []
    for path in sorted((root / 'linux/qt').glob('*')):
        if path.suffix not in ('.cpp', '.h', '.ui'):
            continue
        relative = str(path.relative_to(root))
        raw = path.read_bytes()
        files.append({'path': relative, 'sha256': hashlib.sha256(raw).hexdigest()})
        text = raw.decode('utf-8')
        if path.suffix == '.ui':
            for node in ET.fromstring(text).iter('string'):
                source = node.text or ''
                if source and node.get('notr') != 'true':
                    messages.setdefault(source, []).append({'path': relative, 'line': 0, 'call': 'ui'})
            continue
        tokens = [token for token in TOKENS.finditer(text) if token.lastgroup != 'comment']
        for index, token in enumerate(tokens):
            name = token.group()
            if name not in ('tr', 'translate', 'QT_TRANSLATE_NOOP'):
                continue
            if index + 1 >= len(tokens) or tokens[index + 1].group() != '(':
                continue
            # The catalog implementation has its own translate member. Only
            # inventory Qt's static translate calls, not internal lookup logic.
            if name == 'translate' and (index < 3 or tokens[index - 1].group() != ':'
                    or tokens[index - 2].group() != ':'
                    or tokens[index - 3].group() not in ('QCoreApplication', 'QApplication')):
                continue
            cursor = index + 2
            source, end = literal_argument(tokens, cursor)
            if name != 'tr':
                if not source or end >= len(tokens) or tokens[end].group() != ',':
                    continue
                source, end = literal_argument(tokens, end + 1)
            location = {'path': relative, 'line': text.count('\n', 0, token.start()) + 1, 'call': name}
            complete = end < len(tokens) and tokens[end].group() in (',', ')')
            if source and complete:
                messages.setdefault(source, []).append(location)
            else:
                dynamic.append(location)
    result = {
        'schema_version': 1,
        'scope': 'Literal tr(), Qt static translate(), QT_TRANSLATE_NOOP and translatable UI strings in linux/qt.',
        'limits': 'This is a source inventory, not a runtime reachability claim. Backend/provider diagnostics and native-only text need separate review. No translation is invented or copied by this tool.',
        'literal_source_count': len(messages),
        'literal_occurrence_count': sum(len(locations) for locations in messages.values()),
        'dynamic_calls': dynamic,
        'files': files,
        'messages': [{'source': source, 'locations': messages[source]} for source in sorted(messages)],
    }
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({key: result[key] for key in ('literal_source_count', 'literal_occurrence_count', 'dynamic_calls')}))


if __name__ == '__main__':
    main()
