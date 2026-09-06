#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a scanner test target: grey ramp, hairlines, text, corner marks."""
import sys

parts = ["0.5 w"]
# Frame, so cropping and margins are visible.
parts.append("20 20 555 802 re S")
# Corner marks.
for x, y in ((30, 30), (545, 30), (30, 792), (545, 792)):
    parts.append(f"0 0 0 rg {x - 5} {y - 5} 10 10 re f")
# Grey ramp, 11 steps 0..100 %.
for i in range(11):
    g = i / 10.0
    parts.append(f"{g:.2f} g {60 + i * 45} 640 45 60 re f")
# Solid black block.
parts.append("0 0 0 rg 60 540 200 60 re f")
# Hairlines of increasing width.
for i, w in enumerate((0.25, 0.5, 0.75, 1.0, 1.5, 2.0)):
    y = 500 - i * 14
    parts.append(f"0 0 0 RG {w} w 60 {y} m 400 {y} l S")
# Fine and coarse text.
parts.append("BT /F1 7 Tf 60 400 Td (7pt ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789) Tj ET")
parts.append("BT /F1 12 Tf 60 370 Td (12pt scanner reference target - pantum-open) Tj ET")
parts.append("BT /F1 24 Tf 60 320 Td (24pt Reference) Tj ET")
# Checkerboard for halftone inspection.
for row in range(8):
    for col in range(16):
        if (row + col) % 2 == 0:
            parts.append(f"0 0 0 rg {60 + col * 20} {150 + row * 20} 20 20 re f")

content = "\n".join(parts).encode()
objs = [b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R "
        b"/Resources << /Font << /F1 5 0 R >> >> >>",
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
out, offs = bytearray(b"%PDF-1.4\n"), []
for i, o in enumerate(objs, 1):
    offs.append(len(out))
    out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
x = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for o in offs:
    out += b"%010d 00000 n \n" % o
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
open(sys.argv[1], "wb").write(bytes(out))
print("wrote", sys.argv[1])
