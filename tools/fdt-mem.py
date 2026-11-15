#!/usr/bin/env python3
"""Find flattened device trees in a file and print their /memory nodes.

  tools/fdt-mem.py ramdump.bin [image.bin ...]

Scans for the FDT magic, walks the structure block, and reports every
memory@... node's reg (base, size).  Used to see what memory size the guest is
being told, and where the blob sits.
"""

import struct
import sys

MAGIC = 0xD00DFEED


def align4(n):
    return (n + 3) & ~3


def parse(blob, at):
    magic, totalsize, off_struct, off_strings, _, _, _, _, _ = \
        struct.unpack(">9I", blob[at:at + 36])
    if magic != MAGIC:
        return None
    strings = blob[at + off_strings:at + totalsize]
    p = at + off_struct
    path = []
    nodes = []
    while True:
        tok = struct.unpack(">I", blob[p:p + 4])[0]
        p += 4
        if tok == 1:                     # BEGIN_NODE
            end = blob.index(b"\0", p)
            name = blob[p:end].decode("ascii", "replace")
            p = align4(end + 1)
            path.append(name)
        elif tok == 2:                   # END_NODE
            if path:
                path.pop()
        elif tok == 3:                   # PROP
            ln, nameoff = struct.unpack(">II", blob[p:p + 8])
            p += 8
            name = strings[nameoff:strings.index(b"\0", nameoff)].decode()
            val = blob[p:p + ln]
            p = align4(p + ln)
            nodes.append(("/".join(x for x in path if x), name, val))
        elif tok == 4:
            pass
        elif tok == 9:                   # END
            break
        else:
            return None
    return totalsize, nodes


def report(path):
    data = open(path, "rb").read()
    i = 0
    found = 0
    while True:
        i = data.find(struct.pack(">I", MAGIC), i)
        if i < 0:
            break
        i += 4
        try:
            parsed = parse(data, i - 4)
        except Exception:
            continue
        if not parsed:
            continue
        size, nodes = parsed
        mems = [(n, nm, v) for (n, nm, v) in nodes
                if nm == "reg" and ("memory" in n or "device_type" in n)]
        model = [v for (n, nm, v) in nodes if nm == "model"]
        print(f"{path}: FDT at 0x{i - 4:x} size {size}")
        for v in model:
            print(f"  model = {v.rstrip(chr(0).encode()).decode('ascii','replace')}")
        for n, nm, v in nodes:
            if nm == "reg" and ("memory" in n):
                base, sz = struct.unpack(">QQ", v[:16])
                print(f"  {n}: base=0x{base:x} size=0x{sz:x} "
                      f"({sz >> 20} MiB)")
        found += 1
        if found > 3:
            break


for p in sys.argv[1:]:
    report(p)
