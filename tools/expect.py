#!/usr/bin/env python3
"""Run a QEMU guest and decide pass/fail from its serial output.

Every pattern is checked against the guest's serial log as it grows, so the
run stops the moment the verdict is known - there is no fixed timeout to sit
through.

  tools/expect.py --serial run.log \
      --fail 'unable to allocate fifos' --fail 'Kernel panic' --fail 'Oops' \
      --pass 'root@' --guest 120 \
      -- build/mipsel/qemu-system-mipsel -M mt7621,... -serial file:run.log

A guest that needs keystrokes (OpenWrt waits for Enter before it prints a
prompt) is driven through a serial socket:

  tools/expect.py --serial run.log --sock /tmp/ser.sock \
      --send 'Press Enter=\n' --pass 'root@' ... \
      -- qemu-system-mipsel ... \
         -chardev socket,id=ser,path=/tmp/ser.sock,server=on,wait=off,logfile=run.log \
         -serial chardev:ser

Exit status:
  0  a --pass pattern appeared
  1  a --fail pattern appeared
  2  the guest/wall budget ran out first (no verdict)
  3  QEMU died or could not be started

A pattern may be given as TEXT or as TEXT=N to require N occurrences (useful to
distinguish "one reboot" from "a reboot loop").
"""

import argparse
import os
import re
import signal
import socket
import subprocess
import threading
import sys
import time

TIMESTAMP = re.compile(rb"\[\s*(\d+)\.(\d+)\]")
TAIL = 6


def parse_pattern(spec):
    text, _, count = spec.rpartition("=")
    if text and count.isdigit():
        return text.encode(), int(count)
    return spec.encode(), 1


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--serial", required=True, help="serial log the guest writes")
    ap.add_argument("--fail", action="append", default=[],
                    help="pattern that means failure (repeatable)")
    ap.add_argument("--pass", dest="ok", action="append", default=[],
                    help="pattern that means success (repeatable)")
    ap.add_argument("--guest", type=float, default=0.0,
                    help="guest-time budget in seconds (0 = none)")
    ap.add_argument("--wall", type=float, default=0.0,
                    help="wall-clock budget in seconds (0 = none)")
    ap.add_argument("--stall", type=float, default=180.0,
                    help="stop if the guest prints nothing for this many "
                         "seconds (0 = never); an idle guest stops advancing "
                         "guest time, so a guest-time budget alone can hang")
    ap.add_argument("--show", type=int, default=TAIL,
                    help="log lines to print around the verdict")
    ap.add_argument("--sock", default=None,
                    help="serial socket to write keystrokes to")
    ap.add_argument("--send", action="append", default=[],
                    help="PATTERN=TEXT: write TEXT (\\n for newline) once PATTERN "
                         "appears (repeatable)")
    ap.add_argument("--stderr", default=None,
                    help="file for the command's stderr "
                         "(default: <serial>.err)")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd:
        ap.error("no command given")
    fails = [parse_pattern(p) for p in args.fail]
    oks = [parse_pattern(p) for p in args.ok]
    sends = []
    for spec in args.send:
        pattern, _, text = spec.partition("=")
        sends.append([pattern.lower().encode(),
                      text.encode().decode("unicode_escape").encode("latin-1"),
                      False])
    serial = os.path.expanduser(args.serial)

    if os.path.exists(serial):
        os.unlink(serial)
    # the guest's output must land in the file we watch: either -serial file:X
    # or a chardev with logfile=X
    if not any(a == f"file:{args.serial}" or f"logfile={args.serial}" in a
               or f"logfile={os.path.expanduser(args.serial)}" in a
               for a in cmd):
        print(f"expect: warning: nothing in the command logs to {args.serial}",
              file=sys.stderr)

    errpath = os.path.expanduser(args.stderr or (serial + ".err"))
    errfile = open(errpath, "wb")
    sock = None
    sock_lock = threading.Lock()

    def ensure_sock():
        nonlocal sock
        with sock_lock:
            if sock is None:
                deadline = time.time() + 30
                while True:
                    try:
                        s_ = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        s_.connect(os.path.expanduser(args.sock))
                        sock = s_
                        break
                    except OSError:
                        if time.time() > deadline:
                            raise
                        time.sleep(0.1)
            return sock

    def drain():
        # A socket chardev stalls the guest when nobody reads it, so attach
        # immediately and discard what the console prints (the log file is
        # our copy of it).
        try:
            s_ = ensure_sock()
        except OSError:
            return
        while True:
            try:
                if not s_.recv(65536):
                    return
            except OSError:
                return

    if args.sock:
        threading.Thread(target=drain, daemon=True).start()

    proc = subprocess.Popen(cmd, stdout=errfile, stderr=errfile,
                            start_new_session=True)
    start = time.time()
    last_output = start
    seen = 0
    guest = 0.0
    data = b""
    verdict = None
    code = 2
    reason = "budget"

    try:
        while proc.poll() is None:
            time.sleep(0.25)
            try:
                with open(serial, "rb") as f:
                    f.seek(seen)
                    chunk = f.read()
            except OSError:
                chunk = b""
            if chunk:
                seen += len(chunk)
                data += chunk
                last_output = time.time()
                for m in TIMESTAMP.finditer(chunk):
                    guest = float(m.group(1)) + float(b"0." + m.group(2))
                for item in sends:
                    pat, text, done = item
                    if not done and pat in data.lower():
                        try:
                            ensure_sock().sendall(text)
                            item[2] = True
                        except OSError:
                            item[2] = True
                for pat, need in fails:
                    if data.count(pat) >= need:
                        verdict, code, reason = pat, 1, "fail"
                        break
                if not verdict:
                    for pat, need in oks:
                        if data.count(pat) >= need:
                            verdict, code, reason = pat, 0, "pass"
                            break
            if verdict:
                break
            if args.guest and guest >= args.guest:
                reason = f"guest budget {args.guest:g}s"
                break
            if args.wall and time.time() - start > args.wall:
                reason = f"wall budget {args.wall:g}s"
                break
            if args.stall and time.time() - last_output > args.stall:
                reason = f"no output for {args.stall:g}s"
                break
    finally:
        alive = proc.poll() is None
        if alive:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
        elif proc.returncode not in (0, None):
            code, reason = 3, f"qemu exited {proc.returncode}"

    errfile.close()
    text = data.decode("utf-8", "replace").replace("\r", "")
    lines = [ln for ln in text.splitlines() if ln.strip()]
    verdict_line = next((ln for ln in lines
                         if verdict and verdict.decode() in ln), "")
    print(f"{reason.upper()}: guest={guest:.1f}s wall={time.time() - start:.0f}s"
          + (f"  [{verdict.decode()}]" if verdict else ""))
    if verdict_line:
        print(f"  {verdict_line}")
    tail = lines[-args.show:] if args.show > 0 else []
    for ln in tail:
        print("  " + ln)
    return code


if __name__ == "__main__":
    sys.exit(main())
