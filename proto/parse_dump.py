#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarise an usbshim dump: one line per ASP frame, image data collapsed."""
import re
import sys
from collections import Counter

CMD_RE = re.compile(r"^(OUT|IN |CTRL|CLAIM|ALTSET)")
HEX_RE = re.compile(r"^  ([0-9a-f]{4})  ((?:[0-9a-f]{2} )+)")


def frames(path):
    cur = None
    for line in open(path):
        m = CMD_RE.match(line)
        if m:
            if cur:
                yield cur
            cur = {"head": line.rstrip(), "bytes": bytearray(), "trunc": 0}
            continue
        h = HEX_RE.match(line)
        if h and cur is not None:
            cur["bytes"] += bytes.fromhex(h.group(2).replace(" ", ""))
            continue
        if line.startswith("  ...") and cur is not None:
            cur["trunc"] = int(line.split()[1])
    if cur:
        yield cur


def be32(b, off):
    return int.from_bytes(b[off:off + 4], "big")


def main(path):
    counts = Counter()
    for f in frames(path):
        b = f["bytes"]
        head = f["head"]
        if b[:3] == b"ASP":
            cmd = be32(b, 4)
            payload_len = be32(b, 20)
            extra = ""
            if len(b) > 32:
                body = b[32:]
                extra = " payload=" + " ".join(
                    f"[{i//4}]={be32(body, i)}" for i in range(0, min(len(body), 104), 4)
                    if be32(body, i) != 0
                )
            print(f"{head[:3]} cmd=0x{cmd:02x} plen={payload_len} total={len(b)}{extra}")
            counts[(head[:3].strip(), cmd)] += 1
        elif head.startswith(("CLAIM", "ALTSET", "CTRL")):
            print(head)
        else:
            n = len(b) + f["trunc"]
            print(f"{head[:3]} <data {n} bytes>")
            counts[(head[:3].strip(), "data")] += 1

    print("\n=== summary ===")
    for (dirn, cmd), n in sorted(counts.items(), key=lambda x: str(x[0])):
        c = f"0x{cmd:02x}" if isinstance(cmd, int) else cmd
        print(f"  {dirn} {c}: {n}")


main(sys.argv[1])
