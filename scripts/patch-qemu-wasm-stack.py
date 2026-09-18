#!/usr/bin/env python3
from pathlib import Path
import sys
MARKER='panda-wasm-tci-thread-scratch-v1'
OLD="""    uint64_t stack[(TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE)
                   / sizeof(uint64_t)];"""
NEW="""    /* panda-wasm-tci-thread-scratch-v1
     * Asyncify can suspend TCI execution. Reuse the same-sized per-thread
     * heap buffer allocated by init_wasm32 instead of retaining ~6 KiB on
     * every suspended pthread C stack frame.
     */
    uint64_t *stack = ctx.stack;"""
def patch(root: Path):
    path=root/'tcg'/'wasm32.c'; text=path.read_text()
    allocation='ctx.stack = g_malloc(TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE);'
    if allocation not in text: raise SystemExit('qemu-wasm ctx.stack allocation shape changed')
    if MARKER in text:
        if OLD in text or NEW not in text: raise SystemExit('invalid existing TCI scratch patch')
        return False
    if text.count(OLD)!=1: raise SystemExit('qemu-wasm TCI scratch declaration shape changed')
    path.write_text(text.replace(OLD,NEW)); return True
if __name__=='__main__':
    if len(sys.argv)!=2: raise SystemExit('usage: patch-qemu-wasm-stack.py <qemu-wasm-root>')
    print('patched' if patch(Path(sys.argv[1])) else 'already-patched')
