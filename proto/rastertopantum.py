#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reference re-implementation of the Pantum M6500/M6507 CUPS filter.

Reads a CUPS raster (8 bit/pixel grayscale, "3SaR" v3 uncompressed) on stdin
and writes the ZjStream the printer expects on stdout.  This is a prototype
whose only job is to prove the format: the JBIG entity is produced by
jbig-kit's `pbmtojbg` (identical output to the vendor filter), so a real C
filter would link libjbig instead.

Usage:
    rastertopantum.py [options] < in.raster > out.zjs

Options (CUPS-style key=value, taken from the 5th argv like a real filter):
    Density=0|2|4        toner density, default 2
    TonerMode=True       toner save; forces the density to 0
    DPI1200=True         2 bits per pixel, doubled JBIG width
    InputSlot=Auto|ManualFeed|AutoFeed   tray, ignored for non-plain media
    NegativePrint=True   invert the halftone decision
    ImageRotation=True   rotate the page 180 degrees

Paper size and media type are NOT options here: like the vendor filter, they
are read out of the CUPS raster header (PageSize in points, cupsMediaType).

Not implemented here: manual duplex (the ZJT_2600N_PAUSE chunk).  The real
filter, src/rastertopantum.c, does that as well.
"""

import os
import struct
import subprocess
import sys
import tempfile

UEL = b"\x1b%-12345X"
MAGIC = b"JZJZ"
SIG = 0x5a5a

# ZjStream chunk types (identical numbering to foo2zjs)
ZJT_START_DOC = 0
ZJT_END_DOC = 1
ZJT_START_PAGE = 2
ZJT_END_PAGE = 3
ZJT_JBIG_BIH = 4
ZJT_JBIG_BID = 5
ZJT_END_JBIG = 6
ZJT_2600N_PAUSE = 11
ZJT_PANTUM_PARAM = 0x11

# ZJ_ITEM types
ZJI_PAGECOUNT = 0x00
ZJI_DMCOLLATE = 0x01
ZJI_DMDUPLEX = 0x02
ZJI_DMPAPER = 0x03
ZJI_DMCOPIES = 0x04
ZJI_DMDEFAULTSOURCE = 0x05
ZJI_DMMEDIATYPE = 0x06
ZJI_NBIE = 0x07
ZJI_RESOLUTION_X = 0x08
ZJI_RESOLUTION_Y = 0x09
ZJI_RASTER_X = 0x0c
ZJI_RASTER_Y = 0x0d
ZJI_VIDEO_BPP = 0x10
ZJI_VIDEO_X = 0x11
ZJI_VIDEO_Y = 0x12
ZJI_RET = 0x16
ZJI_CUSTOM_WIDTH = 0x18
ZJI_CUSTOM_HEIGHT = 0x19
ZJI_CUSTOM_UNIT = 0x1a
ZJI_PANTUM_DENSITY = 0xff00

# gPaperSizeArray_PT2500_PTM6500 @ 0x616020: the filter looks the PageSize up
# by its size in PostScript points and takes both the ZJI_DMPAPER code and the
# padded video size from this table.  Terminated by width == -1 in the binary.
#   (width_pt, height_pt, video_x, video_y, dmpaper)
PAPER_TABLE = [
    (420, 595, 3264, 4736, 11),    # A5
    (595, 842, 4736, 6784, 9),     # A4
    (612, 792, 4864, 6368, 1),     # Letter
    (612, 1008, 4864, 8192, 5),    # Legal
    (516, 729, 4064, 5856, 13),    # B5
    (279, 540, 2112, 4288, 37),    # EnvMonarch
    (312, 624, 2368, 4992, 27),    # EnvDL
    (459, 649, 3616, 5184, 28),    # EnvC5
    (297, 684, 2240, 5472, 20),    # Com10Envelope
    (283, 420, 2144, 3264, 43),    # Postcard
    (524, 737, 4160, 5920, 263),   # 16K
    (553, 765, 4384, 6144, 264),   # Big16K
    (369, 524, 2848, 4160, 266),   # 32K
    (383, 553, 2976, 4384, 260),   # Big32K
    (298, 420, 2272, 3264, 70),    # A6
    (499, 709, 3936, 5696, 34),    # ISOB5
    (522, 756, 4128, 6080, 7),     # Executive
    (612, 936, 4864, 7584, 14),    # Folio
    (612, 972, 4864, 7872, 269),   # Oficio
    (396, 612, 3072, 4864, 6),     # Statement
    (323, 459, 2464, 3616, 31),    # EnvC6
    (340, 652, 2624, 5216, 270),   # ZL
    (354, 499, 2720, 3936, 271),   # B6
]

# GetPaperArrayIndex tests one size before it searches the table: 421x595 pt is
# A5 lying on its side.  See notes/print-protocol.md section 4.1.
A5_PT = (420, 595)
A5_SIDEWAYS_PT = (421, 595)
DMPAPER_A5_SIDEWAYS = 61       # DMPAPER_A5_ROTATED

# For reference only: the PPD's *MediaType entries set cupsMediaType, which the
# raster header carries into ZJI_DMMEDIATYPE.
#   Plain 1, Transparency 2, Envelope 259, Label 261, Thin paper 263,
#   Cardstock 286, Thick 289

# 16x16 clustered-dot threshold matrix, HT_TABLE_600 @ 0x615f20.
HT_TABLE_600 = [
    0x06, 0x52, 0xe9, 0xfe, 0xfb, 0xe2, 0x46, 0x05, 0x07, 0x55, 0xeb, 0xff, 0xfc, 0xe4, 0x49, 0x05,
    0x37, 0x79, 0xa5, 0xda, 0xd3, 0x98, 0x72, 0x31, 0x39, 0x7a, 0xa7, 0xdc, 0xd5, 0x9c, 0x73, 0x2f,
    0xc7, 0x86, 0x69, 0x29, 0x1b, 0x5f, 0x7e, 0xc0, 0xca, 0x88, 0x6b, 0x2d, 0x1f, 0x61, 0x7b, 0xbe,
    0xf3, 0xb4, 0x12, 0x03, 0x01, 0x0d, 0xac, 0xee, 0xf4, 0xb6, 0x13, 0x03, 0x01, 0x0c, 0xaa, 0xed,
    0xf8, 0xde, 0x41, 0x04, 0x0a, 0x4f, 0xe8, 0xfd, 0xfa, 0xe0, 0x43, 0x04, 0x09, 0x4c, 0xe6, 0xfc,
    0xd0, 0x91, 0x6d, 0x35, 0x3e, 0x77, 0xa2, 0xd8, 0xd2, 0x95, 0x70, 0x33, 0x3b, 0x75, 0x9f, 0xd7,
    0x16, 0x58, 0x83, 0xc5, 0xce, 0x8e, 0x66, 0x26, 0x18, 0x5b, 0x80, 0xc3, 0xcc, 0x8b, 0x64, 0x22,
    0x02, 0x10, 0xb1, 0xf1, 0xf7, 0xbb, 0x15, 0x02, 0x01, 0x0f, 0xaf, 0xf0, 0xf5, 0xb9, 0x14, 0x02,
    0x07, 0x55, 0xeb, 0xff, 0xfc, 0xe4, 0x49, 0x05, 0x06, 0x52, 0xe9, 0xfe, 0xfb, 0xe2, 0x46, 0x05,
    0x39, 0x7a, 0xa7, 0xdc, 0xd5, 0x9c, 0x73, 0x2f, 0x37, 0x79, 0xa5, 0xda, 0xd3, 0x98, 0x72, 0x31,
    0xca, 0x88, 0x6b, 0x2d, 0x1f, 0x61, 0x7b, 0xbe, 0xc7, 0x86, 0x69, 0x29, 0x1b, 0x5f, 0x7e, 0xc0,
    0xf4, 0xb6, 0x13, 0x03, 0x01, 0x0c, 0xaa, 0xed, 0xf3, 0xb4, 0x12, 0x03, 0x01, 0x0d, 0xac, 0xee,
    0xfa, 0xe0, 0x43, 0x04, 0x09, 0x4c, 0xe6, 0xfc, 0xf8, 0xde, 0x41, 0x04, 0x0a, 0x4f, 0xe8, 0xfd,
    0xd2, 0x95, 0x70, 0x33, 0x3b, 0x75, 0x9f, 0xd7, 0xd0, 0x91, 0x6d, 0x35, 0x3e, 0x77, 0xa2, 0xd8,
    0x18, 0x5b, 0x80, 0xc3, 0xcc, 0x8b, 0x64, 0x22, 0x16, 0x58, 0x83, 0xc5, 0xce, 0x8e, 0x66, 0x26,
    0x01, 0x0f, 0xaf, 0xf0, 0xf5, 0xb9, 0x14, 0x02, 0x02, 0x10, 0xb1, 0xf1, 0xf7, 0xbb, 0x15, 0x02,
]

# HT_TABLE_2BIG @ 0x611200 maps a 0..255 level to a 2-bit dot code.
# 0 -> 0, 1..23 -> 1, 24..63 -> 2, 64..255 -> 3.
HT_TABLE_2BIG = [0] + [1] * 23 + [2] * 40 + [3] * 192


# 88-byte vendor blob emitted as chunk type 0x11 (GetTonerParamCmdForPlatformM).
PANTUM_PARAM_BLOB = bytes.fromhex(
    "00000051" "0073006e" "0096007a" "00640096" "00c80064" "00510064"
    "00640064" "00640064" "00640064" "00640064" "00640064" "00640064"
    "00780087" "009600b4" "00c80078" "009600be" "015e0190" "00640064"
)


def chunk(ctype, items=(), raw=b""):
    """Build one ZjStream chunk."""
    body = b"".join(struct.pack(">IHBBI", 12, t, 1, 0, v) for t, v in items)
    body += raw
    return struct.pack(">IIIHH", 16 + len(body), ctype, len(items),
                       len(items) * 12, SIG) + body


class RasterHeader(object):
    """The fields of cups_page_header2_t this filter actually uses."""

    def __init__(self, blob):
        u = lambda off: struct.unpack_from("<I", blob, off)[0]
        self.hw_res_x = u(276)
        self.hw_res_y = u(280)
        self.num_copies = u(340)
        self.page_w_pt = u(352)
        self.page_h_pt = u(356)
        self.width = u(372)
        self.height = u(376)
        self.media_type = u(380)
        self.bits_per_pixel = u(388)
        self.bytes_per_line = u(392)
        self.color_space = u(400)
        self.page_size_name = blob[1732:1796].split(b"\0")[0].decode("ascii", "replace")


def read_raster(fp):
    """Yield (header, page_bytes) for every page in an uncompressed v3 raster."""
    magic = fp.read(4)
    if magic not in (b"3SaR", b"RaS3"):
        raise SystemExit("unsupported CUPS raster magic %r "
                         "(only uncompressed v3 little-endian is handled)" % magic)
    while True:
        blob = fp.read(1796)
        if len(blob) < 1796:
            return
        hdr = RasterHeader(blob)
        if hdr.bits_per_pixel != 8:
            raise SystemExit("expected 8 bits per pixel, got %d" % hdr.bits_per_pixel)
        data = fp.read(hdr.bytes_per_line * hdr.height)
        yield hdr, data


def page_geometry(hdr):
    """Return (dmpaper, video_x, video_y, rotate90) as the vendor filter does."""
    size = (hdr.page_w_pt, hdr.page_h_pt)
    for w, h, vx, vy, code in PAPER_TABLE:
        if size == (w, h):
            return code, vx, vy, False
    if size == A5_SIDEWAYS_PT:
        # The one entry the vendor tests before searching the table: A5 lying
        # on its side.  It answers with the A5 row, announces DMPAPER 61
        # instead of 11, swaps the video size and turns the gray page a
        # quarter clockwise before halftoning.  No PPD asks for this size.
        for w, h, vx, vy, code in PAPER_TABLE:
            if (w, h) == A5_PT:
                return DMPAPER_A5_SIDEWAYS, vy, vx, True
    # Not a known size: announce a custom page and pad the raster ourselves.
    return 0, (hdr.width + 31) & ~31, (hdr.height + 7) & ~7, False


def gray_page(gray, hdr, out_w, out_h, rotate180=False, rotate90=False):
    """The gray page the vendor dithers, as a list of out_w-byte rows.

    The buffer is allocated at the *padded* size and pre-filled with 0xff
    (paper white) before the raster is copied in, which is why the padding
    columns come out white normally but black under NegativePrint.

    Both turns are applied here rather than to the packed bitmap, because the
    halftone screen is looked up by the *output* pixel's coordinates: it stays
    anchored to the sheet rather than to the image.  Turning the packed bitmap
    instead looks the same on bilevel art but differs on anything that is
    actually screened.
    """
    bpl_in, w, h = hdr.bytes_per_line, hdr.width, hdr.height
    # The quarter turn comes last, so the raster is laid out unturned first.
    src_w, src_h = (out_h, out_w) if rotate90 else (out_w, out_h)
    white = b"\xff" * src_w
    rows = []
    for y in range(src_h):
        sy = src_h - 1 - y if rotate180 else y
        if sy < h:
            row = gray[sy * bpl_in:sy * bpl_in + min(w, src_w)]
            row += white[len(row):]
        else:
            row = white
        rows.append(row[::-1] if rotate180 else row)
    if rotate90:
        # A quarter turn clockwise: out[y][x] = src[src_h - 1 - x][y].
        rows = [bytes(rows[src_h - 1 - x][y] for x in range(out_w))
                for y in range(out_h)]
    return rows


def halftone(gray, hdr, out_w, out_h, negative=False, rotate180=False, depth=1,
             rotate90=False):
    """Threshold 8bpp gray into a packed 1bpp bitmap, 1 = black (toner)."""
    bpl_out = out_w * depth // 8
    out = bytearray(bpl_out * out_h)
    ht = HT_TABLE_600
    ppb = 8 // depth              # source pixels per output byte
    mask = ppb - 1
    rows = gray_page(gray, hdr, out_w, out_h, rotate180, rotate90)
    for y in range(out_h):
        row = rows[y]
        trow = (y & 15) * 16
        thr = bytes(ht[trow + (x & 15)] for x in range(out_w))
        obase = y * bpl_out
        acc = 0
        for x in range(out_w):
            if depth == 1:
                v = 1 if ((row[x] < thr[x]) if not negative
                          else (thr[x] <= row[x])) else 0
            else:
                b = row[x]
                lvl = 0xff if b == 0 else max(0, 0xff - b - thr[x])
                v = HT_TABLE_2BIG[lvl]
                if negative:
                    v = 3 - v
            acc = (acc << depth) | v
            if (x & mask) == mask:
                out[obase + (x // ppb)] = acc
                acc = 0
    return bytes(out)


def jbig_encode(bitmap, w, h, l0=256):
    """Return the 20-byte BIH and the BID for one page.

    jbig-kit's pbmtojbg with these flags is bit-identical to the vendor's
    private copy of jbig.c: sequential, MX=0, stripe height 256,
    options = DPON|TPBON|TPDON|LRLTWO (92), order = SMID|ILEAVE (3).
    """
    with tempfile.TemporaryDirectory() as td:
        pbm = os.path.join(td, "in.pbm")
        jbg = os.path.join(td, "out.jbg")
        with open(pbm, "wb") as f:
            f.write(b"P4\n%d %d\n" % (w, h))
            f.write(bitmap)
        subprocess.run(["pbmtojbg", "-q", "-m", "0", "-s", str(l0),
                        "-p", "92", "-o", "3", pbm, jbg], check=True)
        bie = open(jbg, "rb").read()
    return bie[:20], bie[20:]


def emit_page(out, hdr, bitmap, out_w, out_h, paper, opts):
    # ZJI_DMMEDIATYPE comes straight from cupsMediaType in the raster header
    # (the PPD's *MediaType entries set it); the filter never re-reads the option.
    media = hdr.media_type or 1
    # ZJI_DMDEFAULTSOURCE: anything but plain paper forces the manual tray;
    # otherwise the InputSlot option decides (Auto 0, ManualFeed 1, AutoFeed 2).
    tray = {"auto": 0, "manualfeed": 1, "autofeed": 2}.get(
        opts.get("InputSlot", "Auto").lower(), 0)
    if media != 1:
        tray = 1
    density = int(opts.get("Density", 2))
    if opts.get("TonerMode", "False").lower() == "true":
        density = 0
    depth = 2 if opts.get("DPI1200", "False").lower() == "true" else 1

    items = [
        (ZJI_DMPAPER, paper),
        (ZJI_DMCOPIES, hdr.num_copies or 1),
        (ZJI_DMDEFAULTSOURCE, tray),
        (ZJI_DMMEDIATYPE, media),
        (ZJI_NBIE, 1),
        (ZJI_RESOLUTION_X, hdr.hw_res_x),
        (ZJI_RESOLUTION_Y, hdr.hw_res_y),
        (ZJI_RET, 0),
        (ZJI_RASTER_X, out_w * depth),
        (ZJI_RASTER_Y, out_h),
        (ZJI_VIDEO_BPP, depth),
        (ZJI_VIDEO_X, out_w),
        (ZJI_VIDEO_Y, out_h),
        (ZJI_PANTUM_DENSITY, density),
    ]
    if paper == 0:
        # Custom size: three extra items go straight after ZJI_DMPAPER,
        # expressed in 1/600 inch (device dots).
        items[1:1] = [(ZJI_CUSTOM_UNIT, 0),
                      (ZJI_CUSTOM_WIDTH, hdr.page_w_pt * 600 // 72),
                      (ZJI_CUSTOM_HEIGHT, hdr.page_h_pt * 600 // 72)]
    out.write(chunk(ZJT_START_PAGE, items))

    bih, bid = jbig_encode(bitmap, out_w * depth, out_h)
    out.write(chunk(ZJT_JBIG_BIH, raw=bih))
    for off in range(0, len(bid), 0x10000):
        out.write(chunk(ZJT_JBIG_BID, raw=bid[off:off + 0x10000]))
    out.write(chunk(ZJT_END_JBIG))
    out.write(chunk(ZJT_END_PAGE))


def main(argv):
    opts = {}
    argstr = argv[5] if len(argv) > 5 else " ".join(argv[1:])
    for tok in argstr.replace(",", " ").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            opts[k] = v
    negative = opts.get("NegativePrint", "False").lower() == "true"
    rotate = opts.get("ImageRotation", "False").lower() == "true"

    out = sys.stdout.buffer
    out.write(UEL)
    out.write(MAGIC)
    out.write(chunk(ZJT_PANTUM_PARAM, raw=PANTUM_PARAM_BLOB))
    out.write(chunk(ZJT_START_DOC, [(ZJI_PAGECOUNT, 0),
                                    (ZJI_DMCOLLATE, 0),
                                    (ZJI_DMDUPLEX, 1)]))
    n = 0
    for hdr, data in read_raster(sys.stdin.buffer):
        n += 1
        paper_code, out_w, out_h, sideways = page_geometry(hdr)
        depth = 2 if opts.get("DPI1200", "False").lower() == "true" else 1
        bitmap = halftone(data, hdr, out_w, out_h, negative, rotate, depth,
                          sideways)
        sys.stderr.write("PAGE: %d 1\n" % n)
        emit_page(out, hdr, bitmap, out_w, out_h, paper_code, opts)
    out.write(chunk(ZJT_END_DOC))
    out.write(UEL)
    out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
