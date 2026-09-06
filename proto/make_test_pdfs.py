#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write the PDFs the print-filter regression runs against.

Hand-built rather than produced by a library, so the byte content is fixed:
the regression compares filter output byte for byte, and a PDF that varies
between runs would compare noise.

    make_test_pdfs.py <directory>
"""
import random
import sys


def mkpdf(path, streams, w=595, h=842):
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>", None]
    kids, body, n = [], [], 3
    for c in streams:
        kids.append("%d 0 R" % n)
        body.append((n, ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
                         "/Contents %d 0 R /Resources << >> >>" % (w, h, n + 1)).encode()))
        body.append((n + 1, b"<< /Length %d >>\nstream\n" % len(c) + c + b"\nendstream"))
        n += 2
    objs[1] = ("<< /Type /Pages /Kids [%s] /Count %d >>"
               % (" ".join(kids), len(streams))).encode()
    allobjs = [(1, objs[0]), (2, objs[1])] + body
    out, offs = b"%PDF-1.4\n", {}
    for num, o in allobjs:
        offs[num] = len(out)
        out += b"%d 0 obj\n" % num + o + b"\nendobj\n"
    x, mx = len(out), max(offs) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % mx
    for i in range(1, mx):
        out += b"%010d 00000 n \n" % offs.get(i, 0)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (mx, x)
    open(path, "wb").write(out)


def main(work):
    box = b"0 0 0 rg 100 600 100 100 re f"
    bar = b"0 0 0 rg 200 400 200 50 re f"
    dot = b"0 0 0 rg 50 50 50 50 re f"

    mkpdf(work + "/blank.pdf", [b""])
    mkpdf(work + "/square.pdf", [box])
    mkpdf(work + "/full.pdf", [b"0 0 0 rg 0 0 595 842 re f"])
    mkpdf(work + "/three.pdf", [box, bar, b""])
    mkpdf(work + "/four.pdf", [box, bar, b"", dot])
    mkpdf(work + "/five.pdf", [box, bar, b"", dot, box])

    # Enough small grey rectangles that the compressed page needs more than one
    # JBIG data chunk, which is the only way the multi-chunk path gets tested.
    random.seed(1)
    noise = b"\n".join(
        ("%.2f g %.1f %.1f %.1f %.1f re f"
         % (random.random(), random.uniform(20, 560), random.uniform(20, 810),
            random.uniform(2, 14), random.uniform(2, 14))).encode()
        for _ in range(4000))
    mkpdf(work + "/noise.pdf", [noise])


main(sys.argv[1])
