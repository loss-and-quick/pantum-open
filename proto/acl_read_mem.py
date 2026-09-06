#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read printer memory over the ACL debug channel -- READ ONLY.

    UEL "@PJL ENTER LANGUAGE=ACL\r\n"
    00 ac  00 09  AAAAAAAA  LLLLLLLL  00000000     (16 bytes, big-endian)
    UEL                                            ("execute")
 -> 16-byte reply header + LLLLLLLL bytes of memory

Sub-opcode 0x0009 of opcode 0x00AC is the memory-read command and is the
ONLY sub-opcode this tool can emit.  The same opcode family contains
flash write/erase commands in other firmwares of this lineage (foo2zjs
uses "@PJL ENTER LANGUAGE=ACL" to flash HP LaserJet 1000/1018/1020), so
do not extend this tool with a general sub-opcode argument.

Reply header:

    [0:2]  BE16  0x00AC  -- opcode echo
    [2:4]  BE16  0x0009  -- sub-opcode echo
    [4:6]  BE16  0x0001  -- status, 1 = success
    [6:10] BE32  length of the data that follows
    [10:16]      zero

Throughput is round-trip bound, so use big blocks: 4 KiB requests run at
about 1.5 KiB/s, 256 KiB requests at about 450 KiB/s.

Needs python3 in PATH.  Usage (the device node belongs to group "lp"):

    sg lp -c 'python3 proto/acl_read_mem.py 0xF6000000 0x1000000 flash.bin'
    sg lp -c 'python3 proto/acl_read_mem.py 0 0x2000000 ram-low.bin'

Known regions on the M6507 (see notes/firmware-map.md):

    0x00000000  DRAM, running (decompressed) firmware image
    0xF6000000  SPI NOR flash, packed firmware image
"""

import os
import select
import struct
import sys
import time

DEV = "/dev/usb/lp0"
UEL = b"\x1b%-12345X"
ENTER_ACL = UEL + b"@PJL ENTER LANGUAGE=ACL\r\n"
OPCODE = 0x00AC
SUBOP_READ_MEMORY = 0x0009
BLOCK = 0x40000                 # 256 KiB: the fastest size measured


def _write_all(fd, buf, timeout=15.0):
    deadline = time.time() + timeout
    while buf:
        try:
            buf = buf[os.write(fd, buf):]
        except BlockingIOError:
            if time.time() > deadline:
                raise
            select.select([], [fd], [], 0.05)


def _listen(fd, want, wait, quiet=1.0):
    got, last = b"", None
    deadline = time.time() + wait
    while time.time() < deadline:
        if want and len(got) >= want:
            break
        ready, _, _ = select.select([fd], [], [], 0.05)
        if ready:
            try:
                chunk = os.read(fd, 0x40000)
            except BlockingIOError:
                continue
            except OSError:
                break
            if chunk:
                got += chunk
                last = time.time()
                continue
        if last is not None and time.time() - last > quiet:
            break
    return got


def read_memory(fd, addr, length, wait=60.0):
    """One read-memory round trip.  Returns (header_tuple, data)."""
    _write_all(fd, ENTER_ACL)
    _write_all(fd, struct.pack(">HHIII", OPCODE, SUBOP_READ_MEMORY, addr, length, 0))
    _write_all(fd, UEL)
    reply = _listen(fd, 16 + length, wait)
    if len(reply) < 16:
        return None, b""
    opcode, subop, status, dlen = struct.unpack(">HHHI", reply[0:10])
    if (opcode, subop, status) != (OPCODE, SUBOP_READ_MEMORY, 1):
        return (opcode, subop, status, dlen), b""
    return (opcode, subop, status, dlen), reply[16:16 + length]


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: acl_read_mem.py <base> <length> <outfile>")
    base, total, out = int(sys.argv[1], 0), int(sys.argv[2], 0), sys.argv[3]

    if not os.access(DEV, os.R_OK | os.W_OK):
        sys.exit("%s: no read/write access (the node belongs to group 'lp' -- "
                 "try: sg lp -c '...')" % DEV)

    fd = os.open(DEV, os.O_RDWR | os.O_NONBLOCK)
    done, started = 0, time.time()
    try:
        _listen(fd, 0, 0.3, 0.1)        # drop anything stale
        with open(out, "wb") as sink:
            while done < total:
                want = min(BLOCK, total - done)
                header, data = read_memory(fd, base + done, want)
                if len(data) != want:
                    sys.stderr.write("stopped at +0x%x: header=%s got=%d\n"
                                     % (done, header, len(data)))
                    break
                sink.write(data)
                done += want
                if done % (8 << 20) == 0:
                    sys.stderr.write("  %d MiB in %.1fs\n"
                                     % (done >> 20, time.time() - started))
    finally:
        os.close(fd)
    sys.stderr.write("wrote %d bytes to %s in %.1fs\n"
                     % (done, out, time.time() - started))


if __name__ == "__main__":
    main()
