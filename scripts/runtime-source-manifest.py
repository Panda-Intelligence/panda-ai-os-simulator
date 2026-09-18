#!/usr/bin/env python3
"""Record a locally built runtime identity; no guest/publication approval."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def source(root):
    revision = git(root, 'rev-parse', 'HEAD').decode().strip()
    patch = git(root, 'diff', '--binary', 'HEAD')
    untracked = []
    for raw_name in git(root, 'ls-files', '--others', '--exclude-standard', '-z').split(b'\0'):
        if not raw_name:
            continue
        relative = raw_name.decode('utf-8')
        path = root / relative
        if path.is_symlink():
            raise ValueError(f'untracked source symlink requires explicit review: {relative}')
        if not path.is_file() or path.stat().st_size > 64 * 1024 * 1024:
            raise ValueError(f'unbounded/non-regular source input: {relative}')
        raw = path.read_bytes()
        untracked.append({'path': relative, 'bytes': len(raw),
                          'sha256': hashlib.sha256(raw).hexdigest()})
    untracked.sort(key=lambda entry: entry['path'])
    untracked_identity = json.dumps(untracked, sort_keys=True, separators=(',', ':')).encode()
    return {'commit': revision, 'worktreePatchSha256': hashlib.sha256(patch).hexdigest(),
            'untrackedSourceSha256': hashlib.sha256(untracked_identity).hexdigest(),
            'untrackedSources': untracked, 'modified': bool(patch or untracked)}


def record(output, qemu, native, simulator, image):
    files = []
    for name in ('qemu-system-xtensa.js', 'qemu-system-xtensa.wasm', 'qemu-system-xtensa.worker.js'):
        path = output / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size < 1:
            raise ValueError(f'missing regular runtime artifact: {name}')
        raw = path.read_bytes()
        if name.endswith('.wasm') and raw[:8] != b'\0asm\x01\0\0\0':
            raise ValueError('invalid WebAssembly module header')
        files.append({'path': name, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
    image_id = subprocess.check_output(['docker', 'image', 'inspect', '--format', '{{.Id}}', image], text=True).strip()
    data = {'schemaVersion': 1, 'kind': 'local-runtime-build', 'qemu': source(qemu),
            'nativeOverlay': source(native), 'simulator': source(simulator),
            'toolchainImageId': image_id, 'artifacts': files,
            'guestBootQualified': False, 'publicationApproved': False}
    (output / 'runtime-source.json').write_text(json.dumps(data, indent=2) + '\n')


if __name__ == '__main__':
    try:
        if len(sys.argv) != 6:
            raise ValueError('usage: runtime-source-manifest.py output qemu native simulator image')
        record(*(Path(p) for p in sys.argv[1:5]), sys.argv[5])
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
