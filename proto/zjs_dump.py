#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Dump the ZjStream produced by the Pantum CUPS filter.

The stream is a sequence of big-endian chunks wrapped in PJL UEL escapes:

    ESC %-12345X            UEL
    "JZJZ"                  Pantum stream magic
    <chunk> <chunk> ...
    ESC %-12345X            UEL

Chunk header (16 bytes, big-endian):
    u32 size        total chunk size including this header
    u32 type        ZJT_*
    u32 nitems      number of ZJ_ITEM records in the payload
    u16 item_bytes  nitems * 12
    u16 signature   always 0x5a5a

Item record (12 bytes, big-endian):
    u32 size        always 12
    u16 type        ZJIT_*
    u8  param
    u8  reserved
    u32 value
"""

import sys
import struct

UEL = b"\x1b%-12345X"
MAGIC = b"JZJZ"

ZJT = {
    0x00: "START_DOC",
    0x01: "END_DOC",
    0x02: "START_PAGE",
    0x03: "END_PAGE",
    0x04: "JBIG_BIH",
    0x05: "JBIG_BID",
    0x06: "END_JBIG",
    0x07: "SIGNATURE",
    0x08: "RAW_IMAGE",
    0x09: "START_PLANE",
    0x0a: "END_PLANE",
    0x0b: "PAUSE",
    0x11: "PT_PARAM",
}

ZJIT = {
    0x00: "PAGECOUNT",
    0x01: "DMCOLLATE",
    0x02: "DMDUPLEX",
    0x03: "DMPAPER",
    0x04: "DMCOPIES",
    0x05: "DMDEFAULTSOURCE",
    0x06: "DMMEDIATYPE",
    0x07: "NBIE",
    0x08: "RESOLUTION_X",
    0x09: "RESOLUTION_Y",
    0x0a: "OFFSET_X",
    0x0b: "OFFSET_Y",
    0x0c: "RASTER_X",
    0x0d: "RASTER_Y",
    0x0e: "COLLATE",
    0x0f: "QUANTITY",
    0x10: "VIDEO_BPP",
    0x11: "VIDEO_X",
    0x12: "VIDEO_Y",
    0x13: "INTERLACE",
    0x14: "PLANE",
    0x15: "PALETTE",
    0x16: "RET",
    0x17: "ECONOMODE",
    0x18: "PT_CUSTOM_WIDTH",
    0x19: "PT_CUSTOM_HEIGHT",
    0x1a: "PT_CUSTOM_UNIT",
    0xff00: "PT_DENSITY",
}


def parse(buf):
    out = []
    pos = 0
    if buf.startswith(UEL):
        out.append((0, len(UEL), "UEL", None))
        pos = len(UEL)
    if buf[pos:pos + 4] == MAGIC:
        out.append((pos, 4, "MAGIC 'JZJZ'", None))
        pos += 4
    while pos < len(buf):
        if buf[pos:pos + len(UEL)] == UEL:
            out.append((pos, len(UEL), "UEL", None))
            pos += len(UEL)
            continue
        if pos + 16 > len(buf):
            out.append((pos, len(buf) - pos, "TRAILING", buf[pos:]))
            break
        size, ctype, nitems = struct.unpack(">III", buf[pos:pos + 12])
        ibytes, sig = struct.unpack(">HH", buf[pos + 12:pos + 16])
        if sig != 0x5a5a or size < 16:
            out.append((pos, len(buf) - pos, "UNPARSED", buf[pos:]))
            break
        body = buf[pos + 16:pos + size]
        items = []
        for i in range(nitems):
            rec = body[i * 12:(i + 1) * 12]
            isize, itype, iparam, irsv = struct.unpack(">IHBB", rec[:8])
            ival = struct.unpack(">I", rec[8:12])[0]
            items.append((isize, itype, iparam, irsv, ival))
        raw = body[nitems * 12:]
        out.append((pos, size, "CHUNK", (ctype, nitems, ibytes, items, raw)))
        pos += size
    return out


def fmt(buf, show_raw=True):
    lines = []
    for off, size, kind, info in parse(buf):
        if kind != "CHUNK":
            lines.append("%06x  %-12s len=%d %s" %
                         (off, kind, size, info.hex() if isinstance(info, bytes) else ""))
            continue
        ctype, nitems, ibytes, items, raw = info
        lines.append("%06x  CHUNK size=%-5d type=0x%02x %-14s items=%d raw=%d" %
                     (off, size, ctype, ZJT.get(ctype, "?"), nitems, len(raw)))
        for isize, itype, iparam, irsv, ival in items:
            lines.append("            item type=0x%04x %-18s param=%d value=%d (0x%x)" %
                         (itype, ZJIT.get(itype, "?"), iparam, ival, ival))
        if raw and show_raw:
            lines.append("            raw: " + raw.hex())
    return "\n".join(lines)


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    print(fmt(data, show_raw="--noraw" not in sys.argv))
