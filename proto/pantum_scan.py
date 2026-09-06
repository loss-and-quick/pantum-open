#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Standalone scan over the Pantum "ASP" bulk protocol, reimplemented from a
capture of the proprietary backend. No SANE, no vendor blob.

Frame layout (all integers big-endian):

    struct asp_header {          /* 32 bytes, both directions */
        char     magic[4];       /* "ASP\x01"                  */
        uint32_t command;
        uint32_t reserved[3];
        uint32_t payload_len;    /* bytes following the header */
        uint32_t reserved2[2];
    };

Every command is answered with a header carrying the same command code; a
non-zero payload_len means the payload follows as its own bulk read. After
CMD_START the device pushes frames on its own until the page ends.

The scanner only ever produces 8-bit samples: greyscale is one plane, colour
is three planes sent line-interleaved, and lineart is greyscale thresholded
by the host (the vendor backend does this in convertGraytoBW).
"""
import argparse
import struct
import sys

import usb.core
import usb.util

VENDOR, PRODUCT = 0x232B, 0x0E20
IFACE = 1  # interface 0 is the printer (usblp); the scanner is vendor-specific
EP_OUT, EP_IN = 0x02, 0x82

MAGIC = b"ASP\x01"
HEADER_LEN = 32

CMD_HELLO = 0x00
CMD_BYE = 0x01
CMD_START = 0x02
CMD_ABORT = 0x03
CMD_ABORTED = 0x04
CMD_DATA = 0x05
CMD_GET_WINDOW = 0x06
CMD_SET_WINDOW = 0x07
CMD_PROBE = 0x08
CMD_PARAMS = 0x09
CMD_PAGE_BEGIN = 0x0B
CMD_JOB_END = 0x0C
CMD_SCAN_END = 0x0D
CMD_PAGE_END = 0x0E
CMD_ADF_STATUS = 0x0F

# Status word of a reply frame.
STATUS = {0: "ok", 2: "busy", 5: "no paper", 6: "jam", 7: "jam", 8: "cover open"}

# 32-bit word offsets into the 100-byte window descriptor.
W_RESOLUTION = 3
W_LENGTH = 18  # window height, hundredths of an inch
W_WIDTH = 19  # window width, hundredths of an inch
W_MODE = 14     # must be 0x100; the device rejects the descriptor with 0 here
W_COLOUR = 24  # 1 = three colour planes, 0 = single grey plane

# The device advertises a slightly larger travel than it will accept in a
# window (1180x856); the vendor backend clamps to this and so do we.
MAX_LENGTH, MAX_WIDTH = 1169, 850

BLOCK_GREY, BLOCK_COLOUR = 0x06, 0x0E


class Pantum:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.dev = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
        if self.dev is None:
            sys.exit("Pantum 232b:0e20 not found")
        # Interface 0 stays with usblp so that printing keeps working.
        usb.util.claim_interface(self.dev, IFACE)

    def close(self):
        usb.util.release_interface(self.dev, IFACE)

    def log(self, *a):
        if self.verbose:
            print(*a, file=sys.stderr)

    def read_exact(self, length):
        """Bulk reads come back in transfer-sized chunks, not whole payloads."""
        buf = bytearray()
        while len(buf) < length:
            buf += self.dev.read(EP_IN, length - len(buf), timeout=30000)
        return bytes(buf)

    def send(self, command, payload=b""):
        head = MAGIC + struct.pack(">IIIIIII", command, 0, 0, 0, len(payload), 0, 0)
        self.dev.write(EP_OUT, head + payload, timeout=10000)
        self.log(f"-> 0x{command:02x} +{len(payload)}")

    def recv(self):
        head = self.read_exact(HEADER_LEN)
        if head[:4] != MAGIC:
            raise IOError(f"bad magic: {head[:8].hex()}")
        command = struct.unpack(">I", head[4:8])[0]
        plen = struct.unpack(">I", head[20:24])[0]
        payload = self.read_exact(plen) if plen else b""
        self.log(f"<- 0x{command:02x} +{len(payload)}")
        return command, payload

    def command(self, cmd, payload=b""):
        self.send(cmd, payload)
        reply, data = self.recv()
        if reply != cmd:
            raise IOError(f"expected 0x{cmd:02x}, got 0x{reply:02x}")
        return data


def words(buf):
    return list(struct.unpack(f">{len(buf) // 4}I", buf))


def pack_words(w):
    return struct.pack(f">{len(w)}I", *w)


def scan(dev, resolution, mode, out):
    dev.command(CMD_HELLO)
    dev.command(CMD_PROBE)

    raw = dev.command(CMD_GET_WINDOW)
    window = words(raw)
    dev.log("window in: ", raw.hex(" ", 4))

    colour = mode == "color"
    window[W_RESOLUTION] = resolution
    window[W_MODE] = 0x100
    window[W_LENGTH] = min(window[W_LENGTH], MAX_LENGTH)
    window[W_WIDTH] = min(window[W_WIDTH], MAX_WIDTH)
    window[W_COLOUR] = 1 if colour else 0
    dev.log("window out:", pack_words(window).hex(" ", 4))
    dev.command(CMD_SET_WINDOW, pack_words(window))
    dev.command(CMD_START)

    planes, stride, height = {}, 0, 0
    while True:
        cmd, payload = dev.recv()
        if cmd == CMD_DATA:
            h = words(payload[:24])
            first, count, nplanes, stride = h[1], h[2], h[3], h[4]
            body = payload[24:]
            # stride counts pixels, not bytes: a colour line is stride pixels of
            # nplanes interleaved samples each.
            row_bytes = stride * nplanes
            for i in range(count):
                planes[first + i] = body[i * row_bytes:(i + 1) * row_bytes]
            height = max(height, first + count)
        elif cmd == CMD_JOB_END:
            break  # a job ends 0x0e, 0x0d, 0x0c — the job frame is the last one
        elif cmd in (CMD_PAGE_END, CMD_SCAN_END):
            dev.log(f"end frame 0x{cmd:02x}")
        elif cmd == CMD_ABORTED:
            dev.command(CMD_BYE)
            sys.exit("scanner aborted the job (busy, or the previous job is still finishing)")

    dev.command(CMD_BYE)
    if not planes:
        sys.exit("no image data received")

    with open(out, "wb") as f:
        if mode == "color":
            f.write(f"P6\n{stride} {height}\n255\n".encode())
        elif mode == "gray":
            f.write(f"P5\n{stride} {height}\n255\n".encode())
        else:
            f.write(f"P5\n{stride} {height}\n255\n".encode())
        nplanes = 3 if mode == "color" else 1
        blank = b"\xff" * stride * nplanes
        for i in range(height):
            row = planes.get(i, blank)
            if mode == "color":
                # Samples arrive as B, R, G per pixel.
                f.write(bytes(row[j + k] for j in range(0, len(row), 3) for k in (1, 2, 0)))
            else:
                f.write(row)
    print(f"wrote {out}: {stride}x{height} {mode}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-r", "--resolution", type=int, default=75)
    p.add_argument("-m", "--mode", choices=("gray", "color"), default="gray")
    p.add_argument("-o", "--out", default="scan.pnm")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args()
    dev = Pantum(verbose=a.verbose)
    try:
        scan(dev, a.resolution, a.mode, a.out)
    finally:
        dev.close()


main()
