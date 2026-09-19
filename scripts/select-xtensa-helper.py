#!/usr/bin/env python3
"""Select the single merged project exception-helper translation unit."""
from pathlib import Path
import re
import sys


def select(root: Path) -> bool:
    directory = root / 'target/xtensa'
    merged = (directory / 'exc_helper.c').read_text()
    # These definitions exist in the reviewed merged file, not a declaration
    # header or upstream's exception-only file. Fail instead of losing symbols.
    required = ('xtensa_register_core', 'xtensa_breakpoint_handler',
                'xtensa_cpu_do_unaligned_access', 'xtensa_cpu_tlb_fill',
                'xtensa_cpu_do_transaction_failed', 'xtensa_runstall')
    for name in required:
        if not re.search(r'\b' + name + r'\s*\([^;{}]*\)\s*\{', merged, re.S):
            raise ValueError(f'merged Xtensa helper missing definition: {name}')
    path = directory / 'meson.build'
    text = path.read_text()
    if text.count("'exc_helper.c'") < 1:
        raise ValueError('missing merged exc_helper.c source registration')
    if text.count("'helper.c'") > 1:
        raise ValueError('ambiguous helper.c source registration')

    # Overlaying reviewed Xtensa sources can encounter a qemu-wasm tree where
    # helper.c has already been replaced once before this selector runs. That
    # can leave the *same* reviewed exc_helper.c registered twice. Normalize
    # duplicate registrations only after validating the merged translation unit
    # above; never select between distinct helper implementations implicitly.
    updated_lines = []
    seen_exc_helper = False
    for line in text.splitlines(keepends=True):
        if re.match(r"^\s*'exc_helper\.c',\s*$", line.rstrip('\n')):
            if seen_exc_helper:
                continue
            seen_exc_helper = True
        if re.match(r"^\s*'helper\.c',\s*$", line.rstrip('\n')):
            continue
        updated_lines.append(line)
    updated = ''.join(updated_lines)
    if updated.count("'exc_helper.c'") != 1:
        raise ValueError('failed to normalize merged exc_helper.c source registration')
    if updated == text:
        return False
    path.write_text(updated)
    return True


if __name__ == '__main__':
    try:
        if len(sys.argv) != 2:
            raise ValueError('usage: select-xtensa-helper.py <qemu-source-root>')
        print('Xtensa merged helper selected' if select(Path(sys.argv[1])) else 'Xtensa merged helper already selected')
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
