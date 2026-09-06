#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure colour channel registration in a scan.

A colour-sequential CIS captures the three channels at slightly different
positions, which would show up as a fixed offset between them. A fixed
physical offset doubles in pixels when the resolution doubles; measurement
noise grows like sqrt(2). Comparing two resolutions therefore tells the two
apart, which a single measurement cannot.

Needs a target with sharp edges in saturated colours — black text is useless
here, since all three channels change together at its edges.

    measure_channel_align.py <scan.pnm> [left top width height]
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        sys.exit(f"{path}: not a binary PPM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    return fields[0], fields[1], data[pos + 1:]


def plane(pixels, width, height, index, box):
    x0, y0, w, h = box
    out = bytearray(w * h)
    for y in range(h):
        row = (y0 + y) * width
        base = y * w
        for x in range(w):
            out[base + x] = pixels[((row + x0 + x) * 3) + index]
    return out


def mad(a, b, w, h, dx, dy):
    """Mean absolute difference of b shifted by (dx, dy) against a."""
    total = count = 0
    ax0, bx0 = max(0, -dx), max(0, dx)
    ay0, by0 = max(0, -dy), max(0, dy)
    span_x, span_y = w - abs(dx), h - abs(dy)
    for y in range(span_y):
        ra, rb = (ay0 + y) * w + ax0, (by0 + y) * w + bx0
        for x in range(span_x):
            total += abs(a[ra + x] - b[rb + x])
            count += 1
    return total / count if count else float("inf")


def best_shift(a, b, w, h, axis, radius=4):
    scores = {}
    for d in range(-radius, radius + 1):
        scores[d] = mad(a, b, w, h, d if axis == "x" else 0,
                        d if axis == "y" else 0)
    best = min(scores, key=scores.get)
    # Parabolic interpolation around the minimum gives the sub-pixel optimum.
    sub = best
    if -radius < best < radius:
        l, c, r = scores[best - 1], scores[best], scores[best + 1]
        denom = l - 2 * c + r
        if denom:
            sub = best + 0.5 * (l - r) / denom
    return best, sub, scores[best]


def main():
    path = sys.argv[1]
    width, height, pixels = read_ppm(path)
    if len(sys.argv) > 5:
        box = tuple(int(v) for v in sys.argv[2:6])
    else:
        box = (0, 0, width, height)
    x0, y0, w, h = box
    print(f"{path}: {width}x{height}, window {w}x{h} at {x0},{y0}")

    planes = {n: plane(pixels, width, height, i, box)
              for i, n in enumerate(("R", "G", "B"))}
    for first, second in (("R", "G"), ("R", "B"), ("G", "B")):
        for axis in ("x", "y"):
            whole, sub, score = best_shift(planes[first], planes[second],
                                           w, h, axis)
            print(f"  {first}->{second} {axis}: best {whole:+d} px, "
                  f"sub-pixel {sub:+.2f} px, mad {score:.2f}")


main()
