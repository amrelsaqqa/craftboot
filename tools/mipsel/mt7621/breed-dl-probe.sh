#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Show what Config1.Dl actually does to Breed's DMA allocator.
#
# The allocator wrapper at 0x8ffd29e4 (Breed 1.1 git-676bfd4, HiWiFi HC5962
# image) computes
#
#     n       = size - 1 + 2 * line
#     p       = malloc(n)
#     aligned = (p + line - 1) & ~(line - 1)
#     memset(aligned, 0, n)
#
# so the clear starts at `aligned` and runs n bytes: whenever malloc does not
# return a line-aligned pointer, the tail of that clear lands past the end of
# the allocation.
#
# This script breaks *after* the malloc call in that wrapper and prints
#
#     line size     (cached from Config1.Dl by Breed itself)
#     n             (size - 1 + 2 * line)
#     malloc(n)     (NULL means the heap is already inconsistent)
#
# Run it at Dl=2 and Dl=4: at Dl=2 the first two 1 KiB requests succeed; at
# Dl=4 the first succeeds and the second returns NULL - the clear of the first
# one overran its block.
#
#   tools/mipsel/mt7621/breed-dl-probe.sh [image]
#
# Addresses are for the image named below; re-derive them for another build.
cd "$(dirname "$0")/../../.." || exit 1
BIN=${QEMU:-build/mipsel/qemu-system-mipsel}
IMG=${1:-${IMAGE:-$HOME/full.bin}}
[ -x "$BIN" ] || { echo "missing $BIN (make TARGET=mipsel build)" >&2; exit 1; }
[ -f "$IMG" ] || { echo "missing image $IMG (set IMAGE=…)" >&2; exit 1; }

probe() {
    dl=$1
    pkill qemu-system-mip >/dev/null 2>&1
    sleep 1
    MT7621_DL=$dl "$BIN" -M mt7621,board=lg-gapm7200 -m 256M \
        -kernel "$IMG" -display none -serial "file:/tmp/breed-dl$dl.log" \
        -S -gdb tcp:127.0.0.1:1247 >/dev/null 2>&1 &
    qpid=$!
    sleep 2
    python3 - "$dl" <<'PY'
import socket, sys

s = socket.create_connection(('127.0.0.1', 1247), timeout=10)
s.settimeout(10)

def send(p):
    p = p.encode()
    s.sendall(b'$' + p + b'#' + ('%02x' % (sum(p) & 0xff)).encode())

def wait(t=10):
    s.settimeout(t)
    d = b''
    while True:
        c = s.recv(65536)
        if not c:
            return ''
        d += c
        i = d.find(b'$')
        if i >= 0:
            j = d.find(b'#', i)
            if j > 0 and len(d) >= j + 3:
                s.sendall(b'+')
                return d[i + 1:j].decode('latin1')

def reg(n):
    send('p%x' % n)
    v = wait()
    return int.from_bytes(bytes.fromhex(v), 'little') if v and v[0] != 'E' else None

def r32(a):
    send('m%x,4' % a)
    v = wait()
    return int.from_bytes(bytes.fromhex(v), 'little') if v and v[0] != 'E' else None

send('qSupported:multiprocess+'); wait()
send('Z0,8ffd2a34,4'); wait()      # after malloc(n) in the DMA wrapper
dl = sys.argv[1]
shown = 0
for _ in range(12):
    send('c')
    pkt = wait(12)
    if not pkt.startswith('T'):
        break
    n, ret = reg(16), reg(2)
    gp = reg(28)
    base = r32(gp + 0x18)
    line = r32(base - 0x39dc)
    print('Dl=%s line=%s n=0x%x malloc(n)=0x%x' % (dl, line, n, ret))
    shown += 1
    if shown >= 6:
        break
    send('z0,8ffd2a34,4'); wait(); send('s'); wait(); send('Z0,8ffd2a34,4'); wait()
s.close()
PY
    sleep 1
    kill "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null
}

probe 2
probe 4
