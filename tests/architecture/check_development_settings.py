#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Check the internal development setting registry against source use."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
REGISTRY = ROOT / 'src/host/anygm_development_settings.def'
ENTRY = re.compile(r'ANYGM_DEVELOPMENT_SETTING\(([A-Z0-9_]+), '
                   r'(DIAGNOSTIC|OVERRIDE|TUNING|TEST), (QUERY|VM_TEXT)\)')


def main() -> None:
    lines = [line for line in REGISTRY.read_text().splitlines()
             if line.startswith('ANYGM_DEVELOPMENT_SETTING')]
    entries = [ENTRY.fullmatch(line) for line in lines]
    if not entries or any(entry is None for entry in entries):
        raise SystemExit('invalid development setting registry entry')
    names = [entry[1] for entry in entries]
    if names != sorted(set(names)):
        raise SystemExit('development setting registry needs unique sorted names')

    literal_names = set()
    vm_text = set()
    for path in (ROOT / 'src').rglob('*'):
        if path.suffix not in {'.c', '.h', '.cpp'}:
            continue
        source = path.read_text(errors='replace')
        literal_names.update(re.findall(r'"GML_([A-Z0-9_]+)"', source))
        vm_text.update(re.findall(r'\bGML_VM_SETTING_([A-Z0-9_]+)\b', source))
    vm_text.discard('COUNT')
    if vm_text != {entry[1] for entry in entries if entry[3] == 'VM_TEXT'}:
        raise SystemExit('VM text settings differ from their registered ownership')
    used = literal_names | vm_text
    known = set(names)
    if used != known:
        raise SystemExit(f'development setting registry drift: missing {sorted(used - known)}, '
                         f'unused {sorted(known - used)}')
    print(f'development settings: {len(names)} names, {len(vm_text)} VM text snapshots')


if __name__ == '__main__':
    main()
