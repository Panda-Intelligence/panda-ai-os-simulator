#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

def validate_elf(data: bytes):
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4:7] != b"\x01\x01\x01":
        raise ValueError("example requires ELF32 little-endian; use an ESP32-S3-specific compiler")
    kind,machine=struct.unpack_from("<HH",data,16)
    entry=struct.unpack_from("<I",data,24)[0]
    if kind!=2 or machine!=94 or not 0x40370000<=entry<0x403e0000:
        raise ValueError("example is not an executable Xtensa IRAM guest")
    return entry

if __name__=="__main__":
    try:
        entry=validate_elf(Path(sys.argv[1]).read_bytes())
        print(f"guest ELF verified: ESP32-S3 little-endian, entry=0x{entry:x}")
    except (OSError,ValueError,IndexError) as error:
        print(str(error),file=sys.stderr);sys.exit(1)
