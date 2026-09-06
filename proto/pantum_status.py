#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Talk to a Pantum M6500-series printer over the usblp character device.

One tool with subcommands, because every one of them needs the same
transport and the same knowledge about how this firmware misbehaves:

    id          IEEE-1284 Device ID via the usblp LPIOC_GET_DEVICE_ID ioctl
    info        every "@PJL INFO" query the device might answer
    status      human-readable summary of what the device reports
    watch       poll plus "@PJL USTATUS DEVICE=ON", print on every change
    drain       read what is already buffered, without writing anything
    probe-job   send a prepared print stream and record what comes back

Needs python3 in PATH.  The device node belongs to group "lp", so:

    cd <repository root>
    sg lp -c 'python3 proto/pantum_status.py status'
    sg lp -c 'python3 proto/pantum_status.py watch --log dumps/status/run.txt'

Which subcommands modify device state:

  * "id", "info", "status" and "drain" are strictly read-only.  The ioctl
    is a kernel-side query, and "@PJL INFO" / "@PJL DINQUIRE" are PJL
    *information* requests: no SET, DEFAULT, INITIALIZE, RESET or USTATUS
    is sent, so neither the configuration nor the unsolicited-status
    subscription is touched.
  * "watch" turns the unsolicited-status subscription on and off again.
  * "probe-job" prints.  It puts paper through the engine.

Two properties of this firmware are load-bearing and were expensive to
find out; the code below depends on both:

  * while the device waits for paper it STOPS ACCEPTING WRITES.  Every
    os.write() then fails with BlockingIOError (EAGAIN), so status must
    not be polled in that state -- only read.  An active subscription
    keeps delivering reports on its own, which is why it is worth having.
  * for the same reason "@PJL USTATUSOFF" can fail on the first attempt.
    Unsubscribing has to be retried, or the printer stays subscribed
    after the program is gone and keeps chattering into the buffer.

TONER: "@PJL INFO SUPPLIES" answers "?" on this model and no PJL
information request carries a supply level.  The level does exist, but
only in the asynchronous "@PJL USTATUS DEVICE" report as "TONER=<pct>",
which is why "watch" and "probe-job" subscribe and "status" does not.
See notes/status-protocol.md and notes/error-catalogue.md.
"""

import argparse
import fcntl
import os
import re
import select
import signal
import struct
import sys
import time

DEV = "/dev/usb/lp0"

UEL = b"\x1b%-12345X"           # PJL Universal Exit Language escape
FF = "\x0c"                     # PJL replies are terminated by a form feed


# --------------------------------------------------------------------------
# Transport
# --------------------------------------------------------------------------

def read_device_id(path=DEV):
    """IEEE-1284 Device ID via the usblp LPIOC_GET_DEVICE_ID ioctl.

    From drivers/usb/class/usblp.c:
        IOCNR_GET_DEVICE_ID = 1
        LPIOC_GET_DEVICE_ID(len) = _IOC(_IOC_READ, 'P', 1, len)
    _IOC layout (asm-generic/ioctl.h): dir(2) type(8) nr(8) size(14),
    shifts 30/8/0/16.

    The buffer comes back as a 2-byte big-endian length (counting those
    two bytes themselves) followed by the ASCII string.  Returns the
    (string, raw buffer, declared length) triple, because "id" prints
    all three and everything else only wants the string.
    """
    length = 1024
    buf = bytearray(length)
    request = (2 << 30) | (ord("P") << 8) | (1 << 0) | (length << 16)
    with open(path, "rb+", buffering=0) as f:
        fcntl.ioctl(f, request, buf, True)
    raw = bytes(buf)
    total = struct.unpack(">H", raw[0:2])[0]
    return raw[2:total].decode("latin1", errors="replace"), raw, total


def listen(fd, wait, quiet=None, on_text=None):
    """Collect bytes from the printer's bulk-IN pipe.

    Stops after `wait` seconds, or `quiet` seconds after the last byte if
    `quiet` is given.  `on_text` is called with each decoded chunk as it
    arrives, so a long listen can report while it is still running.
    """
    got, last = b"", None
    deadline = time.time() + wait
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError:
                break
            if chunk:
                got += chunk
                last = time.time()
                if on_text:
                    on_text(chunk.decode("latin1", errors="replace"))
                continue
        if quiet is not None and last is not None and time.time() - last > quiet:
            break
    return got


def write_all(fd, buf, timeout=5.0):
    """Write every byte, waiting out the short EAGAIN stalls.

    Raises BlockingIOError if the device is still refusing after
    `timeout` -- which is what happens once it is blocked waiting for
    paper, and is a state observation rather than a bug.
    """
    deadline = time.time() + timeout
    while buf:
        try:
            buf = buf[os.write(fd, buf):]
        except BlockingIOError:
            if time.time() > deadline:
                raise
            select.select([], [fd], [], 0.05)


def pjl(request, path=DEV, wait=3.0, quiet=0.6):
    """Send one PJL request on a fresh handle and return the reply text.

    The request is wrapped in UEL on both sides, exactly as the stock
    Pantum CUPS filter wraps its own PJL headers.
    """
    payload = UEL + b"@PJL " + request.encode() + b"\r\n" + UEL
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    try:
        listen(fd, 0.2, 0.05)           # discard anything left over
        write_all(fd, payload)
        reply = listen(fd, wait, quiet)
    finally:
        os.close(fd)
    return reply.decode("latin1", errors="replace")


def unsubscribe(fd, deadline_seconds=150.0):
    """Drop the USTATUS subscription, retrying for as long as it takes.

    The write is refused while the printer is blocked (see the module
    docstring), and a subscription left on outlives this process: the
    device keeps sending reports at whoever opens the node next.
    """
    until = time.time() + deadline_seconds
    while time.time() < until:
        try:
            os.write(fd, UEL + b"@PJL USTATUSOFF\r\n" + UEL)
            listen(fd, 1.0)
            return True
        except BlockingIOError:
            listen(fd, 1.0)
        except OSError:
            return False
    return False


def install_sigterm_handler():
    """Turn SIGTERM into SystemExit so `finally` blocks still run.

    Without this a killed process never unsubscribes and the printer is
    left broadcasting.
    """
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))


def require_access(path, write=True):
    mode = os.R_OK | os.W_OK if write else os.R_OK
    if not os.access(path, mode):
        sys.exit("%s: no %s access (the node belongs to group 'lp' -- "
                 "try: sg lp -c '...')"
                 % (path, "read/write" if write else "read"))


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

# PJL status codes seen on or expected from this family.  The numeric
# classes come from the firmware's own PJL classifier (constants 5000 /
# 10000 / 20000 / 30000 / 40000 / 50000 in the RAM image, see
# notes/error-catalogue.md); the individual texts are the standard PJL
# readback assignments and are HYPOTHESES until observed on the device.
PJL_STATUS = {
    "10001": "ready (online)",
    "10002": "ready (idle) -- the normal resting code of this firmware",
    "10003": "warming up",
    "10004": "self test",
    "10005": "reset / processing a job",
    "10006": "toner low",
    "10023": "printing",
    "10024": "printing test page",
    "40000": "sleep",
    "40021": "front cover open",
    "40600": "cartridge removed / not installed",
    "42000": "feed jam -- also what an empty tray reports",
}


def status_class(code):
    if not code.isdigit():
        return "unparsable"
    n = int(code)
    if n < 10000:
        return "informational (<10000)"
    if n < 20000:
        return "device status / ready class (10000-19999)"
    if n < 30000:
        return "background paper-mount / media class (20000-29999)"
    if n < 40000:
        return "auto-continuable error (30000-39999)"
    if n < 50000:
        return "operator intervention required (40000-49999)"
    return "unclassified (>=50000)"


def describe(code):
    text = PJL_STATUS.get(code)
    return ("%s -- %s" % (text, status_class(code))) if text else status_class(code)


def first_value(reply, key):
    """Pull 'KEY = value' or 'KEY=value' out of a PJL reply body."""
    m = re.search(rf"^{re.escape(key)}\s*=\s*(.*)$", reply, re.MULTILINE)
    return m.group(1).strip() if m else None


def body_lines(reply):
    """Reply body: everything after the echoed request line, minus the FF."""
    text = reply.replace(FF, "")
    lines = [l.rstrip("\r") for l in text.split("\n")]
    if lines and lines[0].startswith("@PJL"):
        lines = lines[1:]
    return [l for l in lines if l.strip()]


def parse_fields(reply):
    """Every KEY=VALUE in a reply body, echoed request lines dropped."""
    fields = {}
    for line in reply.replace(FF, "").splitlines():
        line = line.strip()
        if line.startswith("@PJL") or not line:
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            fields[k.strip()] = v.strip()
    return fields


def parse_device_id(s):
    fields = {}
    for part in s.split(";"):
        if ":" in part:
            k, v = part.split(":", 1)
            fields[k.strip()] = v.strip()
    return fields


# --------------------------------------------------------------------------
# Subcommand: id
# --------------------------------------------------------------------------

def cmd_id(args):
    """IEEE-1284 Device ID.  A pure read served by the kernel's usblp."""
    require_access(args.device)
    text, raw, total = read_device_id(args.device)
    print("raw bytes (first 96):", raw[:96])
    print("declared total length (incl. 2-byte header):", total)
    print("DEVICE ID STRING:")
    print(text)
    return 0


# --------------------------------------------------------------------------
# Subcommand: info
# --------------------------------------------------------------------------

# A query the device does not implement answers "?"; a query it ignores
# answers nothing at all.  Both are recorded, because which is which is
# itself a finding.
INFO_QUERIES = ("ID", "STATUS", "CONFIG", "VARIABLES", "MEMORY", "PAGECOUNT",
                "USTATUS", "FILESYS", "SUPPLIES")


def cmd_info(args):
    require_access(args.device)
    fd = os.open(args.device, os.O_RDWR | os.O_NONBLOCK)
    try:
        for query in INFO_QUERIES:
            write_all(fd, UEL + f"@PJL INFO {query}\r\n".encode() + UEL)
            buf, deadline = b"", time.time() + args.wait
            while time.time() < deadline:
                r, _, _ = select.select([fd], [], [], 0.3)
                if r:
                    try:
                        chunk = os.read(fd, 4096)
                    except BlockingIOError:
                        continue
                    except OSError:
                        break
                    if chunk:
                        buf += chunk
                        deadline = time.time() + args.quiet_time
            print(f"=== @PJL INFO {query} ===")
            print(buf.decode("latin1", errors="replace").strip() or "(no reply)")
    finally:
        os.close(fd)
    return 0


# --------------------------------------------------------------------------
# Subcommand: status
# --------------------------------------------------------------------------

def cmd_status(args):
    """Human-readable summary.  Provenance is noted next to every field."""
    require_access(args.device)

    # -- identity ---------------------------------------------------------
    # Source: usblp ioctl.  Static string burnt into the device, always
    # available even while the printer is busy.
    devid, devid_err = None, None
    try:
        devid = read_device_id(args.device)[0]
    except OSError as e:
        devid_err = str(e)
    # Source: "@PJL INFO ID".  Same model string, but proves the PJL
    # interpreter is alive and answering.
    r_id = pjl("INFO ID", args.device)

    # -- state ------------------------------------------------------------
    # Source: "@PJL INFO STATUS" -> "CODE=<n>".  This is the only live
    # state value an information request exposes.
    r_status = pjl("INFO STATUS", args.device)
    code = first_value(r_status, "CODE") or ""

    # -- counters ---------------------------------------------------------
    # Source: "@PJL DINQUIRE PAGECOUNT" -- the *default* value of the
    # PAGECOUNT variable.  On this firmware "@PJL INFO PAGECOUNT" and
    # "@PJL INQUIRE PAGECOUNT" both come back empty, and INFO VARIABLES
    # reports 'PAGECOUNT = Bad Value', i.e. the counter is declared but
    # not populated.  Treat the number below as unreliable.
    r_pages_d = pjl("DINQUIRE PAGECOUNT", args.device)
    r_pages_i = pjl("INFO PAGECOUNT", args.device)
    pages_d = body_lines(r_pages_d)
    pages_i = body_lines(r_pages_i)

    # -- config -----------------------------------------------------------
    # Source: "@PJL INFO MEMORY" and "@PJL INFO CONFIG".
    r_mem = pjl("INFO MEMORY", args.device)
    r_cfg = pjl("INFO CONFIG", args.device)
    r_vars = pjl("INFO VARIABLES", args.device)

    # -- print ------------------------------------------------------------
    print("=== Pantum printer status ===")
    print(f"device node        : {args.device}")

    if devid:
        f = parse_device_id(devid)
        print(f"manufacturer       : {f.get('MFG', '?')}")
        print(f"model              : {f.get('MDL', '?')}")
        print(f"command languages  : {f.get('CMD', '?')}")
        print(f"class              : {f.get('CLS', '?')}")
    else:
        print(f"device id          : unavailable ({devid_err})")

    pjl_id = body_lines(r_id)
    print(f"PJL responds as    : {pjl_id[0] if pjl_id else '(no reply)'}")

    if code:
        print(f"state              : CODE={code} -- {describe(code)}")
    else:
        print("state              : (no CODE in reply)")

    mem = first_value(r_mem.replace("@PJL INFO MEMORY  =", "MEMORY ="), "MEMORY")
    if mem is None:
        m = re.search(r"MEMORY\s*=\s*(\d+)", r_mem)
        mem = m.group(1) if m else None
    if mem and mem.isdigit():
        print(f"free memory        : {int(mem):,} bytes")

    langs = []
    m = re.search(r"LANGUAGE \[[^\]]*\]\s*(.*?)(?=^\S|\Z)", r_cfg,
                  re.MULTILINE | re.DOTALL)
    if m:
        langs = [l.strip() for l in m.group(1).splitlines() if l.strip()]
    if langs:
        print(f"languages offered  : {', '.join(langs)}")

    if pages_d or pages_i:
        shown = (pages_i or pages_d)[0]
        note = "" if pages_i else "  (DINQUIRE default, NOT a live counter)"
        print(f"page count         : {shown or '(empty)'}{note}")
    else:
        print("page count         : not reported by this firmware")

    print("toner level        : not exposed by any @PJL INFO query")
    print("                     (it arrives only as TONER= in an asynchronous")
    print("                      USTATUS report -- use the 'watch' subcommand;")
    print("                      see notes/status-protocol.md)")

    if args.raw:
        print("\n--- verbatim replies ---")
        for name, reply in [("INFO ID", r_id), ("INFO STATUS", r_status),
                            ("INFO MEMORY", r_mem), ("INFO CONFIG", r_cfg),
                            ("INFO VARIABLES", r_vars),
                            ("INFO PAGECOUNT", r_pages_i),
                            ("DINQUIRE PAGECOUNT", r_pages_d)]:
            print(f"\n@PJL {name}\n{reply}")
    return 0


# --------------------------------------------------------------------------
# Subcommand: watch
# --------------------------------------------------------------------------

def cmd_watch(args):
    """Record what the printer reports while an operator provokes states.

    Two sources at once, because neither alone is enough:

      * asynchronous "@PJL USTATUS DEVICE" reports, which fire on a change
        and are the only place TONER= appears;
      * a slow "@PJL INFO STATUS" poll, which catches states that were
        already settled by the time the listening started.

    Subscribing changes device state, so the subscription is dropped on
    every way out, Ctrl-C and SIGTERM included.
    """
    install_sigterm_handler()
    require_access(args.device)

    log = open(args.log, "a", buffering=1) if args.log else None
    fd = os.open(args.device, os.O_RDWR | os.O_NONBLOCK)
    start = time.time()
    last = {}
    subscribed = False

    def emit(line):
        out = "%7.1fs  %s" % (time.time() - start, line)
        print(out, flush=True)
        if log:
            log.write(out + "\n")

    def note(text):
        # Report a field when its VALUE CHANGES, not the first time the
        # field is seen.  A state that recurs later in the session is
        # exactly what this is for, so a "seen once, never again" filter
        # would throw away the interesting part.
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith(("@PJL INFO", "@PJL USTATUS")):
                continue
            key = line.split("=")[0] if "=" in line else line
            if not args.all and last.get(key) == line:
                continue
            last[key] = line
            if line.startswith("CODE="):
                emit("%s   %s" % (line, describe(line[5:].strip())))
            else:
                emit(line)

    try:
        if not args.no_subscribe:
            write_all(fd, UEL + b"@PJL USTATUS DEVICE=ON\r\n" + UEL)
            subscribed = True
            emit("-- subscribed to USTATUS DEVICE, provoke states now --")
        else:
            emit("-- polling only, no subscription --")
        while args.seconds <= 0 or time.time() - start < args.seconds:
            try:
                write_all(fd, UEL + b"@PJL INFO STATUS\r\n" + UEL, timeout=1.0)
            except BlockingIOError:
                # The device is blocked (waiting for paper, most likely) and
                # refuses writes.  Polling is impossible in that state; the
                # subscription keeps delivering on its own, so just listen.
                note("(device is not accepting writes -- listening only)")
            note(listen(fd, args.interval).decode("latin1", errors="replace"))
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        dropped = True
        if subscribed:
            dropped = unsubscribe(fd)
        os.close(fd)
        if log:
            log.close()
        if subscribed:
            print("subscription dropped" if dropped
                  else "WARNING: still subscribed", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# Subcommand: drain
# --------------------------------------------------------------------------

def cmd_drain(args):
    """Read whatever the printer has already reported, writing nothing.

    Useful exactly when the device has stopped accepting writes: anything
    sent now would fail with EAGAIN, but reports from an earlier
    subscription are sitting in the buffer waiting to be read.
    """
    require_access(args.device, write=False)
    fd = os.open(args.device, os.O_RDONLY | os.O_NONBLOCK)
    try:
        out, deadline = b"", time.time() + args.seconds
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.5)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                except (OSError, BlockingIOError):
                    continue
                if chunk:
                    out += chunk
                    deadline = time.time() + args.quiet_time
        print(out.decode("latin1", errors="replace").strip()
              or "(nothing buffered)")
    finally:
        os.close(fd)
    return 0


# --------------------------------------------------------------------------
# Subcommand: probe-job
# --------------------------------------------------------------------------

def cmd_probe_job(args):
    """Send a prepared print stream and record what the device reports.

    THIS PRINTS.  It is how the states that are invisible while idle get
    observed: "out of paper" is one of them, because the firmware only
    notices at pick time, so the only way to see the code is to ask it to
    print with nothing in the tray.  It is also how TONER= is obtained,
    since the device only emits a USTATUS report when something happens.

    The stream is whatever the filter produces, e.g.
        rastertopantum 1 user title 1 '' < page.raster > job.zjs
    """
    install_sigterm_handler()
    require_access(args.device)

    data = open(args.job, "rb").read()
    subs = [s.strip().upper() for s in args.ustatus.split(",") if s.strip()]
    for s in subs:
        if s not in ("DEVICE", "JOB", "PAGE"):
            sys.exit("unknown USTATUS class: %s (device, job, page)" % s)

    start = time.time()
    last = {}

    def emit(line):
        print("%7.1fs  %s" % (time.time() - start, line), flush=True)

    def note(text):
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith(("@PJL INFO", "@PJL USTATUS")):
                continue
            key = line.split("=")[0] if "=" in line else line
            if last.get(key) == line:
                continue
            last[key] = line
            if line.startswith("CODE="):
                emit("%s   %s" % (line, describe(line[5:].strip())))
            else:
                emit(line)

    if args.delay > 0:
        print("PREPARE THE DEVICE NOW - sending in %ds" % args.delay, flush=True)
        left = args.delay
        while left > 0:
            print("  %ds left" % left, flush=True)
            time.sleep(min(15, left))
            left -= 15

    fd = os.open(args.device, os.O_RDWR | os.O_NONBLOCK)
    subscribed = False
    try:
        if subs:
            write_all(fd, UEL + b"".join(b"@PJL USTATUS %s=ON\r\n" % s.encode()
                                         for s in subs) + UEL)
            subscribed = True
            note(listen(fd, 3.0).decode("latin1", errors="replace"))

        for i in range(0, len(data), 4096):
            write_all(fd, data[i:i + 4096], timeout=args.write_timeout)
        emit("sent %d bytes" % len(data))

        # Do not poll from here on.  Once the device is waiting for paper it
        # stops accepting writes and every request fails with EAGAIN; the
        # subscription keeps delivering reports on its own, so just listen.
        listen(fd, args.seconds,
               on_text=lambda t: note(t))
    except BlockingIOError:
        emit("device stopped accepting writes mid-stream -- listening instead")
        listen(fd, args.seconds, on_text=lambda t: note(t))
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        dropped = True
        if subscribed:
            dropped = unsubscribe(fd)
        os.close(fd)
        if subscribed:
            print("subscription dropped" if dropped
                  else "WARNING: still subscribed", flush=True)
    return 0


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-d", "--device", default=DEV,
                    help=f"printer character device (default: {DEV})")
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("id", help="IEEE-1284 Device ID via the usblp ioctl")
    p.set_defaults(func=cmd_id)

    p = sub.add_parser("info", help="every @PJL INFO query, verbatim replies")
    p.add_argument("-w", "--wait", type=float, default=3.0,
                   help="seconds to wait for a reply (default 3.0)")
    p.add_argument("-q", "--quiet-time", type=float, default=0.6,
                   help="idle time that ends a reply (default 0.6)")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("status", help="human-readable summary")
    p.add_argument("--raw", action="store_true",
                   help="also dump the verbatim PJL replies")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("watch", help="poll and subscribe, print every change")
    p.add_argument("-i", "--interval", type=float, default=2.0,
                   help="seconds of listening between polls (default 2.0)")
    p.add_argument("-s", "--seconds", type=float, default=0.0,
                   help="stop after this long; 0 means until Ctrl-C (default)")
    p.add_argument("-l", "--log", help="append every observation to this file")
    p.add_argument("-a", "--all", action="store_true",
                   help="print every observation, not only changes")
    p.add_argument("--no-subscribe", action="store_true",
                   help="poll only, leaving the USTATUS subscription untouched "
                        "(keeps the run strictly read-only)")
    p.set_defaults(func=cmd_watch)

    p = sub.add_parser("drain", help="read the buffer, write nothing")
    p.add_argument("-s", "--seconds", type=float, default=20.0,
                   help="how long to keep reading (default 20.0)")
    p.add_argument("-q", "--quiet-time", type=float, default=5.0,
                   help="idle time that ends the read (default 5.0)")
    p.set_defaults(func=cmd_drain)

    p = sub.add_parser("probe-job", help="send a print stream and listen "
                                         "(THIS PRINTS)")
    p.add_argument("job", help="path to a prepared print stream (ZjStream)")
    p.add_argument("-s", "--seconds", type=float, default=90.0,
                   help="how long to listen after sending (default 90.0)")
    p.add_argument("--delay", type=int, default=0,
                   help="countdown before sending, to set the tray up")
    p.add_argument("--ustatus", default="device",
                   help="USTATUS classes to subscribe to, comma separated: "
                        "device,job,page (default: device)")
    p.add_argument("--write-timeout", type=float, default=60.0,
                   help="how long a stalled write may block (default 60.0)")
    p.set_defaults(func=cmd_probe_job)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
