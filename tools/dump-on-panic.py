#!/usr/bin/env python3
"""Connect to a running QEMU's QMP socket and, as soon as the guest logs an
oops or panic, stop the VM, dump all of RAM and the CPU registers.

Usage:
  qemu-system-mipsel ... -qmp unix:/run/user/1000/qmp.sock,server=on,wait=off \
                        -serial file:~/$HOME/run.log
  tools/dump-on-panic.py /run/user/1000/qmp.sock ~/run.log ~/ram.bin

The dump is taken with the VM stopped, so the register window and the memory
image belong to the same instant: disassembling the dump at the reported epc
gives exactly the instruction that faulted.
"""

import json
import os
import socket
import sys
import time

PANIC_MARKERS = (b"Oops[", b"Kernel panic", b"Unable to handle kernel")


class QMP:
    def __init__(self, path, timeout=60.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.buf = b""
        self._read_message()          # greeting
        self.cmd("qmp_capabilities")

    def _read_message(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name, **arguments):
        msg = {"execute": name}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            rep = self._read_message()
            if "event" in rep:
                continue
            if "return" in rep:
                return rep["return"]
            raise RuntimeError(f"QMP error: {rep}")


def wait_for_panic(logpath, offset=0, timeout=3600.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(logpath, "rb") as f:
                f.seek(offset)
                data = f.read()
        except OSError:
            data = b""
        for marker in PANIC_MARKERS:
            if marker in data:
                return marker.decode()
        time.sleep(0.5)
    return None


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    sock, logpath, out = sys.argv[1], os.path.expanduser(sys.argv[2]), \
        os.path.expanduser(sys.argv[3])

    mark = wait_for_panic(logpath)
    print(f"panic marker: {mark}", flush=True)

    q = QMP(sock)
    q.cmd("stop")
    regs = q.cmd("human-monitor-command",
                 **{"command-line": "info registers"})
    with open(out + ".regs", "w") as f:
        f.write(regs if isinstance(regs, str) else json.dumps(regs, indent=2))
    q.cmd("pmemsave", val=0, size=0x10000000, filename=out)
    print(f"wrote {out} and {out}.regs", flush=True)
    q.cmd("quit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
