#!/usr/bin/env python3
"""RipperPS's dashboard background, drawn from nothing but code.

    make-title-art.py OUT.png
    make-title-art.py --wordmark OUT.png [--flat]

A 3840x2160 picture for sce_sys/pic0.dds and pic1.dds (title-art.sh turns it
into BC7): a disc lying in deep space at the heart of a spiral galaxy, torn at
its far rim like the icon, its data pouring out of the tear as a stream of bits
and stars. The RipperPS wordmark sits at the lower left in Montserrat (OFL,
third_party/Montserrat-Medium.ttf), its "PS" lit magenta to fuchsia. Every shape
is drawn here.

--wordmark writes the wordmark alone on a transparent background, cut to its
own size: as it sits on the picture, with its shadow and glow, or with --flat
only the letters.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 3840, 2160
REPO = Path(__file__).resolve().parents[2]
FONT = REPO / "third_party/Montserrat-Medium.ttf"
PALE_SKY, SKY, OCEAN, MAGENTA, FUCHSIA, PERIWINKLE = 0x7dd3fc, 0x0ea5e9, 0x0c4a6e, 0xf0509c, 0xe879f9, 0xa5b4fc

# The disc: its centre, radius and hub on screen, and how it lies in space.
CX, CY, R, HUB = 0.685 * W, 0.60 * H, 0.235 * W, 0.045 * W
TILT, SQUASH = math.radians(-13), 0.40
TEAR_AT = -math.pi / 2  # the far rim, as seen from here


def hex_rgb(value):
    return np.array([(value >> 16) & 255, (value >> 8) & 255, value & 255], dtype=np.float32)


def blur(field, radius):
    """A float field blurred, through Pillow (values kept in 0..1)."""
    img = Image.fromarray(np.clip(field * 255, 0, 255).astype(np.uint8), "L")
    return np.asarray(img.filter(ImageFilter.GaussianBlur(radius)), dtype=np.float32) / 255


def noise(rng, cells, radius):
    """Smooth clouds: random cells scaled up and softened."""
    small = Image.fromarray((rng.random((cells[1], cells[0])) * 255).astype(np.uint8), "L")
    big = small.resize((W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(big, dtype=np.float32) / 255


def to_screen(rho, phi):
    """A point on the disc's plane, in polar form, to screen pixels."""
    dx, dy = rho * np.cos(phi), rho * np.sin(phi) * SQUASH
    return CX + dx * math.cos(TILT) - dy * math.sin(TILT), CY + dx * math.sin(TILT) + dy * math.cos(TILT)


def on_disc():
    """Every pixel's place on the disc's plane: distance from the centre, angle."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    dx, dy = x - CX, y - CY
    rx = dx * math.cos(-TILT) - dy * math.sin(-TILT)
    ry = (dx * math.sin(-TILT) + dy * math.cos(-TILT)) / SQUASH
    return np.sqrt(rx * rx + ry * ry), np.arctan2(ry, rx)


def space(rng):
    """Deep navy, with nebulae in sky blue, fuchsia and a little magenta."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    u, v = x / W, y / H
    img = hex_rgb(0x02070d) * (1 - v[..., None]) + hex_rgb(0x051424) * v[..., None]
    near = np.exp(-(((u - 0.62) * W / H) ** 2 + (v - 0.40) ** 2) / 0.40)
    for colour, k in [(OCEAN, 0.70), (SKY, 0.42), (FUCHSIA, 0.16), (MAGENTA, 0.05)]:
        clouds = 0.55 * noise(rng, (8, 5), 260) + 0.30 * noise(rng, (16, 9), 140) + 0.15 * noise(rng, (32, 18), 70)
        clouds = np.clip((clouds - 0.40) * 2.6, 0, 1) ** 1.8
        img = img + hex_rgb(colour) * (clouds * (0.25 + 0.75 * near) * k)[..., None]
    return img


def galaxy(rng, img):
    """Two spiral arms of stars winding out from the disc's rim, in its plane."""
    stars = np.zeros((H, W), dtype=np.float32)
    colour = np.zeros((H, W, 3), dtype=np.float32)

    def place(rho, phi, bright, rgb):
        x, y = to_screen(rho, phi)
        keep = (x >= 0) & (x < W) & (y >= 0) & (y < H)
        xi, yi = x[keep].astype(int), y[keep].astype(int)
        np.add.at(stars, (yi, xi), bright[keep])
        np.add.at(colour, (yi, xi), rgb[None, :] * bright[keep, None])

    n, turns = 240000, 2.6 * math.pi
    for arm in range(2):
        t = rng.random(n) ** 0.9 * turns
        rho = R * 1.02 * np.exp(0.16 * t) * (1 + np.abs(rng.normal(0, 0.06, n)))
        phi = t + arm * math.pi + 0.6 + rng.normal(0, 0.10 + 0.05 * t / turns, n) * (1 + t / 6)
        bright = (0.15 + rng.random(n) ** 3) * (1.25 - 0.85 * t / turns)
        place(rho, phi, bright, hex_rgb(0xd0f0ff))
        far = t > 0.9 * math.pi
        place(rho[far] * 1.01, phi[far] + 0.02, bright[far] * 0.6, hex_rgb(FUCHSIA if arm else PERIWINKLE))
    # A ring of dust hugging the rim, lit by the disc.
    m = 60000
    rho = R * (1.03 + np.abs(rng.normal(0, 0.07, m)))
    place(rho, rng.random(m) * 2 * math.pi, rng.random(m) ** 2 * 0.6, hex_rgb(PALE_SKY))

    tint = colour / np.maximum(stars, 1e-6)[..., None]
    sharp = np.clip(stars, 0, 1)
    haze = blur(np.clip(stars * 0.08, 0, 1), 40) * 6 + blur(np.clip(stars * 0.2, 0, 1), 10) * 2.5
    glow_tint = np.stack([blur(np.clip(tint[..., c] / 255 * sharp, 0, 1), 30) for c in range(3)], -1)
    glow_tint = glow_tint / np.maximum(blur(sharp, 30), 1e-3)[..., None] * 255
    img = img + glow_tint * np.clip(haze, 0, 1.5)[..., None] * 0.8
    return img + tint * sharp[..., None] * 0.9


def tear_edge(rho, phi):
    """How far each point is from the tear's edge, on the disc (negative: inside it)."""
    start = HUB * 2.3
    t = np.clip((rho - start) / (R - start), 0, 1)                    # 0 at its root, 1 at the rim
    lateral = rho * np.sin(np.angle(np.exp(1j * (phi - TEAR_AT))))
    zig = 2 * np.abs((rho / (R * 0.11)) % 2 - 1) - 1                   # a triangle wave along it
    centre = zig * 0.030 * R * t ** 0.6
    half = 0.11 * R * t ** 1.3 + 2
    ahead = np.cos(phi - TEAR_AT) > 0
    return np.where(ahead & (rho > start), np.abs(lateral - centre) - half, 1e4)


def disc(img):
    """A dark, iridescent disc: rainbow sheen, data tracks, a hub and the tear."""
    rho, phi = on_disc()
    edge = tear_edge(rho, phi)
    inside = (rho < R) & (rho > HUB) & (edge > 0)

    # The sheen: two bright rainbow sectors, as a disc catches a light.
    light = math.radians(205)
    hue = (phi / (2 * math.pi) * 2 + rho / R * 0.35) % 1
    rgb = 0.5 + 0.5 * np.cos(2 * math.pi * (hue[..., None] + np.array([0, 1 / 3, 2 / 3], dtype=np.float32)))
    sector = np.cos(phi - light) ** 2
    sector = sector ** 6 * 0.85 + sector * 0.12
    tracks = 0.92 + 0.08 * np.sin(rho * 0.9) * np.sin(rho * 0.031)
    base = hex_rgb(0x0b1c2c) * (0.85 + 0.3 * (rho / R))[..., None]
    surface = base + rgb * 255 * (sector * tracks)[..., None] * 0.62
    surface = surface + hex_rgb(PALE_SKY) * (np.exp(-np.abs(rho - R) / 6) * 0.9)[..., None]       # the rim's edge
    clamp = (rho > HUB * 1.6) & (rho < HUB * 2.15)                                                 # the clamping ring
    surface = np.where(clamp[..., None], surface * 0.55 + hex_rgb(0x16405c) * 0.6, surface)
    surface = surface + hex_rgb(PALE_SKY) * (np.exp(-np.abs(rho - HUB) / 4) * 0.8)[..., None]

    soft = blur(inside.astype(np.float32), 1.2)[..., None]                                         # smooth edges
    img = img * (1 - soft) + surface * soft

    # The rim's glow, and the tear's hot edges.
    rim = blur((np.abs(rho - R) < 10).astype(np.float32), 26)
    img = img + hex_rgb(SKY) * (rim * 1.6)[..., None]
    hot = np.where(rho < R * 1.01, np.exp(-np.abs(edge) / 7) * (edge > -40), 0)
    img = img + hex_rgb(MAGENTA) * (hot * 1.1)[..., None] + hex_rgb(0xffe0ef) * (hot ** 3 * 0.8)[..., None]
    img = img + hex_rgb(MAGENTA) * (blur(np.clip(hot, 0, 1), 34) * 1.5)[..., None]
    return img


def stream(rng, canvas):
    """The disc's data, pouring out of the tear: bits that shrink and scatter, and dust."""
    x0, y0 = (float(v) for v in to_screen(np.float32(R * 0.97), np.float32(TEAR_AT)))
    pts = [(x0, y0), (x0, y0 - 0.42 * H), (x0 - 0.16 * W, 0.04 * H), (0.30 * W, 0.16 * H)]

    def path(t):
        k = [(1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t ** 2, t ** 3]
        dk = [-3 * (1 - t) ** 2, 3 * (1 - t) * (1 - 3 * t), 3 * t * (2 - 3 * t), 3 * t ** 2]
        x, y = sum(a * p[0] for a, p in zip(k, pts)), sum(a * p[1] for a, p in zip(k, pts))
        dx, dy = sum(a * p[0] for a, p in zip(dk, pts)), sum(a * p[1] for a, p in zip(dk, pts))
        n = np.hypot(dx, dy)
        return x, y, -dy / n, dx / n

    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    # Dust: fine sparks along the stream, widening as it goes.
    n = 30000
    t = rng.random(n)
    x, y, nx, ny = path(t)
    off = rng.normal(0, 1, n) * (18 + 360 * t ** 1.2)
    x, y = x + nx * off, y + ny * off
    palette = [hex_rgb(MAGENTA), hex_rgb(0xffe0ef), hex_rgb(PALE_SKY), hex_rgb(FUCHSIA), hex_rgb(PERIWINKLE)]
    for i in range(n):
        k = rng.integers(0, 2) if t[i] < 0.3 else rng.integers(1, 5)
        a = int(255 * (1 - t[i]) ** 0.7 * rng.uniform(0.3, 1))
        rgb = tuple(int(c) for c in palette[k])
        draw.point((x[i], y[i]), fill=rgb + (a,))
    # Bits: squares, large and magenta near the tear, small and cool far from it.
    m = 170
    t = rng.random(m) ** 1.15
    x, y, nx, ny = path(t)
    off = rng.normal(0, 1, m) * (26 + 260 * t)
    x, y = x + nx * off, y + ny * off
    for i in np.argsort(-t):
        h = (19 * (1 - t[i]) ** 1.3 + 3) * rng.uniform(0.55, 1.15)
        rgb = palette[0] * (1 - t[i]) + (palette[2] if i % 3 else palette[4]) * t[i]
        a = int(255 * min(1, 1.25 - t[i]))
        draw.rounded_rectangle([x[i] - h, y[i] - h, x[i] + h, y[i] + h], radius=h * 0.25,
                               fill=tuple(int(c) for c in rgb) + (a,))
    canvas.alpha_composite(layer.filter(ImageFilter.GaussianBlur(26)))
    canvas.alpha_composite(layer.filter(ImageFilter.GaussianBlur(5)))
    canvas.alpha_composite(layer)


def starfield(rng, img):
    """Stars across the sky; a few bright ones with a soft cross."""
    n = 14000
    x, y = rng.integers(0, W, n), rng.integers(0, H, n)
    bright = 0.12 + 0.88 * rng.random(n) ** 5
    field = np.zeros((H, W), dtype=np.float32)
    np.add.at(field, (y, x), bright * 1.4)
    img = img + 255 * np.clip(field, 0, 1)[..., None] + 255 * blur(np.clip(field * 0.5, 0, 1), 2)[..., None] * 2
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for _ in range(24):
        sx, sy, s = rng.integers(0, W), rng.integers(0, H), rng.uniform(10, 34)
        rgb = tuple(int(c) for c in [hex_rgb(0xffffff), hex_rgb(PALE_SKY), hex_rgb(0xffd6ea)][rng.integers(0, 3)])
        draw.line([(sx - s, sy), (sx + s, sy)], fill=rgb + (150,), width=2)
        draw.line([(sx, sy - s), (sx, sy + s)], fill=rgb + (150,), width=2)
        draw.ellipse([sx - 4, sy - 4, sx + 4, sy + 4], fill=rgb + (255,))
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    out.alpha_composite(layer.filter(ImageFilter.GaussianBlur(5)))
    out.alpha_composite(layer)
    return out


def shade(canvas):
    """Darker towards the lower left, so the wordmark reads."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    d = np.sqrt(((x / W) * 1.2) ** 2 + (y / H - 1.0) ** 2)
    a = (np.clip(1 - d / 0.75, 0, 1) ** 1.5 * 150).astype(np.uint8)
    dark = Image.new("RGBA", (W, H), (3, 2, 10, 255))
    dark.putalpha(Image.fromarray(a, "L"))
    canvas.alpha_composite(dark)


def wordmark(canvas, flat=False):
    """"Ripper" in white, "PS" filled magenta to fuchsia, with a glow of its own."""
    font = ImageFont.truetype(str(FONT), 300)
    x, y = 250, 1700
    head = font.getlength("Ripper")
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x, y + 18), "RipperPS", font=font, fill=(0, 0, 0, 180), anchor="ls")
    if not flat:
        canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(26)))
    ImageDraw.Draw(canvas).text((x, y), "Ripper", font=font, fill=(255, 255, 255, 255), anchor="ls")

    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).text((x + head, y), "PS", font=font, fill=255, anchor="ls")
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    left, width = x + head, font.getlength("PS")
    t = np.clip(((xx - left) / width) * 0.75 + ((y - yy) / 220) * 0.25, 0, 1)[..., None]
    fill = hex_rgb(MAGENTA) * (1 - t) + hex_rgb(FUCHSIA) * t
    gradient = Image.fromarray(fill.astype(np.uint8), "RGB").convert("RGBA")
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    glow.paste(gradient, (0, 0), mask.filter(ImageFilter.MaxFilter(9)))
    if not flat:
        canvas.alpha_composite(glow.filter(ImageFilter.GaussianBlur(34)))
    letters = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    letters.paste(gradient, (0, 0), mask)
    canvas.alpha_composite(letters)


def wordmark_alone(out, flat):
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    wordmark(canvas, flat)
    left, top, right, bottom = canvas.getbbox()
    pad = 0 if flat else 8
    canvas = canvas.crop((max(left - pad, 0), max(top - pad, 0), min(right + pad, W), min(bottom + pad, H)))
    canvas.save(out)
    print(f"wordmark: {out} ({canvas.width}x{canvas.height})")


def main():
    if sys.argv[1] == "--wordmark":
        wordmark_alone(Path(sys.argv[2]), "--flat" in sys.argv[3:])
        return
    out = Path(sys.argv[1])
    rng = np.random.default_rng(7)
    img = space(rng)
    img = galaxy(rng, img)
    img = disc(img)
    img += rng.normal(0, 1.2, img.shape).astype(np.float32)  # grain, so gradients never band
    canvas = starfield(rng, img)
    stream(rng, canvas)
    shade(canvas)
    wordmark(canvas)
    canvas.convert("RGB").save(out)
    print(f"title art: {out} ({W}x{H})")


if __name__ == "__main__":
    main()
