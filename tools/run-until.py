#!/usr/bin/env python3
"""Run a QEMU guest until something interesting happens, then stop it.

Instead of a wall-clock timeout, this watches the serial log: it stops as soon
as the guest panics, prints a marker, or reaches a guest-time budget.  A run
that faults at 35 s of guest time ends in ~35 s of guest time, not after a
fixed timeout.

  tools/run-until.py --guest 75 --serial run.log --pattern 'Kernel panic' \
      --pattern 'root@' -- build/mipsel/qemu-system-mipsel -M mt7621 ...

Prints a one-line summary plus the last interesting log lines.
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import time

TS = re.compile(rb"\[\s*(\d+)\.(\d+)\]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--guest", type=float, default=0.0,
                    help="stop after this much guest time (seconds)")
    ap.add_argument("--serial", required=True)
    ap.add_argument("--pattern", action="append", default=[])
    ap.add_argument("--wall", type=float, default=0.0,
                    help="hard wall-clock cap, seconds (0 = none)")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd:
        ap.error("no command")

    serial = os.path.expanduser(args.serial)
    if os.path.exists(serial):
        os.unlink(serial)
    pats = [p.encode() for p in args.pattern]

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, start_new_session=True)
    start = time.time()
    seen = 0
    guest = 0.0
    hit = None
    try:
        while proc.poll() is None:
            time.sleep(0.4)
            try:
                with open(serial, "rb") as f:
                    f.seek(seen)
                    chunk = f.read()
            except OSError:
                chunk = b""
            if chunk:
                seen += len(chunk)
                for m in TS.finditer(chunk):
                    guest = float(m.group(1)) + float(b"0." + m.group(2))
                for p in pats:
                    if p in chunk:
                        hit = p.decode()
                        break
            if hit:
                break
            if args.guest and guest >= args.guest:
                hit = f"guest {guest:.1f}s"
                break
            if args.wall and time.time() - start > args.wall:
                hit = "wall limit"
                break
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)

    with open(serial, "rb") as f:
        text = f.read().decode("utf-8", "replace").replace("\r", "")
    lines = [ln for ln in text.splitlines() if ln.strip()]
    print(f"stop: {hit or 'process exited'}  guest={guest:.1f}s  "
          f"wall={time.time() - start:.0f}s")
    for pat in ("unable to allocate fifos", "Linux version", "Kernel panic",
                "Oops", "root@", "procd: - init -"):
        n = sum(ln.count(pat) for ln in lines)
        if n:
            print(f"  {pat!r}: {n}")
    for ln in [l for l in lines
               if any(k in l for k in ("epc", "BadVA", "Kernel panic"))][-4:]:
        print("  " + ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
