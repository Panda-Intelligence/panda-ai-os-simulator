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
    if text.count("'exc_helper.c'") != 1:
        raise ValueError('expected exactly one merged exc_helper.c source')
    if text.count("'helper.c'") > 1:
        raise ValueError('ambiguous helper.c source registration')
    updated = re.sub(r"(?m)^\s*'helper\.c',\s*\n", '', text)
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
