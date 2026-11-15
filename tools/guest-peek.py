#!/usr/bin/env python3
"""Inspect a guest RAM dump: disassemble around a kernel address and dump
words, so an oops (epc + registers) can be read exactly.

  tools/guest-peek.py ram.bin dis 80166894 [count]
  tools/guest-peek.py ram.bin mem 82c8ca00 [count]

KSEG0/KSEG1 addresses are mapped back to physical offsets (the kernel runs
from 0x80000000, and the dump starts at physical 0).
"""

import sys

import capstone

BASE = 0x80000000


def to_phys(addr):
    if addr >= 0x80000000 and addr < 0xc0000000:
        return addr & 0x1fffffff
    return addr


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    path, mode, addr = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 24
    data = open(path, "rb").read()
    off = to_phys(addr)

    if mode == "mem":
        chunk = data[off:off + count * 4]
        for i in range(0, len(chunk), 4):
            word = int.from_bytes(chunk[i:i + 4], "little")
            print(f"{BASE + off + i:08x}: {word:08x}")
        return 0

    md = capstone.Cs(capstone.CS_ARCH_MIPS,
                     capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
    start = off - 8 * (count // 2)
    code = data[start:start + count * 4]
    for insn in md.disasm(code, BASE + start):
        mark = " <== epc" if insn.address == addr else ""
        print(f"{insn.address:08x}: {insn.bytes.hex():8s} {insn.mnemonic:8s}"
              f" {insn.op_str}{mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
