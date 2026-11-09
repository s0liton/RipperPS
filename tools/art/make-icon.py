#!/usr/bin/env python3
"""RipperPS's icon, drawn from nothing but code.

    make-icon.py OUT.png [SIZE]

A disc with a jagged tear from its rim to the hub, and the bits it held
rising out of the tear. Flat colours: a sky-blue disc and magenta bits on deep
navy. Drawn 4x larger and scaled down, so edges are smooth. SIZE defaults to 512 (sce_sys/icon0.png).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

BACK = (8, 30, 56)
DISC = (125, 211, 252)
RING = (56, 189, 248)
BITS = (240, 80, 156)
SCALE = 4


def tear(s, c, r, hole):
    """A jagged V, from the rim at the top down to the hub, opening upwards."""
    steps, left, right = 7, [], []
    top, bottom = c[1] - r - 0.02 * s, c[1] - hole * 1.55
    for i in range(steps + 1):
        t = i / steps                                   # 0 at the rim, 1 at the hub
        y = top + (bottom - top) * t
        x = c[0] + (0.032 if i % 2 else -0.032) * s * (1 - t) ** 0.6
        w = 0.13 * s * (1 - t) ** 1.25 + 0.004 * s
        left.append((x - w / 2, y))
        right.append((x + w / 2, y))
    return left + right[::-1]


def main():
    out = Path(sys.argv[1])
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    s = size * SCALE
    c, r, hole = (s / 2, 0.555 * s), 0.315 * s, 0.068 * s
    canvas = Image.new("RGB", (s, s), BACK)
    d = ImageDraw.Draw(canvas)
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=DISC)
    ring = 0.135 * s
    d.ellipse([c[0] - ring, c[1] - ring, c[0] + ring, c[1] + ring], outline=RING, width=int(0.018 * s))
    d.ellipse([c[0] - hole, c[1] - hole, c[0] + hole, c[1] + hole], fill=BACK)
    d.polygon(tear(s, c, r, hole), fill=BACK)

    # The data, rising out of the tear: squares shrinking as they go.
    mouth = c[1] - r
    for dx, dy, k in [(0.000, 0.035, 0.056), (-0.050, -0.040, 0.044), (0.048, -0.055, 0.040),
                      (-0.004, -0.110, 0.032), (-0.078, -0.135, 0.024), (0.070, -0.150, 0.020)]:
        x, y, h = c[0] + dx * s, mouth + dy * s, k * s / 2
        d.rounded_rectangle([x - h, y - h, x + h, y + h], radius=h * 0.28, fill=BITS)

    canvas.resize((size, size), Image.LANCZOS).save(out)
    print(f"icon: {out} ({size}x{size})")


if __name__ == "__main__":
    main()
