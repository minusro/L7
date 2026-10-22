#!/usr/bin/env python3
"""The README's pictures (docs/readme/), from a run of the demo renderer
(tools/demo: the real app's screen, 30 frames a second, in frames.raw):
screenshots at 2x, an annotated screen, the keys and a short GIF.

  make_images.py <demo-out-dir>            (needs Pillow, numpy, ffmpeg)
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

SW, SH = 320, 240
HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.join(HERE, "..", "..", "docs", "readme")
BG, INK, DIM, ACCENT = (12, 12, 14), (236, 236, 236), (150, 150, 156), (0xB5, 0xE3, 0xC4)
FONT = "/System/Library/Fonts/HelveticaNeue.ttc"


def font(size, bold=False):
    return ImageFont.truetype(FONT, size, index=1 if bold else 0)


def frames(out):
    raw = np.memmap(os.path.join(out, "frames.raw"), dtype="<u2", mode="r")
    return raw.reshape(-1, SH, SW)


def rgb(px):
    return np.stack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31], -1).astype(np.uint8)


def shot(f, t, scale=2):
    return Image.fromarray(rgb(f[int(t * 30)])).resize((SW * scale, SH * scale), Image.NEAREST)


def annotated(f):
    """The screen with what each part is (the filter being swept on track 4)."""
    s = shot(f, 46)
    W, H, X, Y = 1500, 660, 430, 40
    img = Image.new("RGB", (W, H), BG)
    img.paste(s, (X, Y))
    d = ImageDraw.Draw(img)
    t, sub = font(24, True), font(20)
    # (label, sub-label, side, label y (or x, below), the point on the screen in its own pixels)
    notes = [
        ("Waveform", "the loop; the part that plays is bright", "L", 130, (60, 104)),
        ("Track rows", "7 at the top, 1 at the bottom", "L", 300, (4, 180)),
        ("Scene", "this scene, of how many", "L", 480, (30, 228)),
        ("Filter", "highpass and lowpass", "R", 130, (225, 104)),
        ("Volume", "level, balance, meter", "R", 260, (272, 165)),
        ("Track button", "filled: playing, outlined: muted", "R", 390, (310, 165)),
        ("Action button", "what A will do", "R", 480, (307, 228)),
        ("Clock", "bar . beat . step", "BL", 820, (160, 228)),
        ("Tempo", "60-240 BPM", "BR", 900, (250, 228)),
    ]
    for label, note, side, at, (px, py) in notes:
        sx, sy = X + px * 2, Y + py * 2
        if side in ("L", "R"):
            ex = X - 24 if side == "L" else X + SW * 2 + 24
            dx, anchor = (-12, "r") if side == "L" else (12, "l")
            d.text((ex + dx, at), label, font=t, fill=ACCENT, anchor=anchor + "s")
            d.text((ex + dx, at + 26), note, font=sub, fill=DIM, anchor=anchor + "s")
            d.line([(ex, at - 8), (sx, sy)], fill=ACCENT, width=2)
        else:
            ly = Y + SH * 2 + 70
            anchor = "r" if side == "BL" else "l"
            d.text((at, ly), label, font=t, fill=ACCENT, anchor=anchor + "s")
            d.text((at, ly + 26), note, font=sub, fill=DIM, anchor=anchor + "s")
            d.line([(at + (8 if side == "BL" else -8), ly - 20), (sx, sy)], fill=ACCENT, width=2)
        d.ellipse((sx - 5, sy - 5, sx + 5, sy + 5), fill=ACCENT, outline=BG, width=2)
    img.save(os.path.join(DEST, "screen.png"), optimize=True)


def keys():
    """The M8's keys as on the device, named as the guide names them."""
    pitch, key = 120, 106
    layout = {"B": (2.18, 0, "OPTION"), "A": (3.16, 0, "EDIT"), "▲": (1, 0.225, ""), "◀": (0, 1.17, ""),
              "▼": (1, 1.17, ""), "▶": (2, 1.17, ""), "SHIFT": (1, 2.32, ""), "START": (2, 2.32, "PLAY")}
    W, H = int(4.16 * pitch) + 80, int(3.32 * pitch) + 60
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    big, small, arrow = font(34, True), font(22, True), ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 36)
    for name, (c, r, alt) in layout.items():
        x, y = 40 + int(c * pitch), 30 + int(r * pitch)
        d.rounded_rectangle((x, y, x + key, y + key), radius=16, fill=(24, 24, 28), outline=(70, 70, 78), width=3)
        f = arrow if name in "▲◀▼▶" else (big if len(name) == 1 else small)
        d.text((x + key / 2, y + key / 2 - (10 if alt else 0)), name, font=f, fill=INK, anchor="mm")
        if alt:
            d.text((x + key / 2, y + key / 2 + 24), alt, font=font(15), fill=DIM, anchor="mm")
    img.save(os.path.join(DEST, "keys.png"), optimize=True)


def gif(f, t0, t1, name):
    """A stretch of the demo as a GIF (15 frames a second, 2x)."""
    with tempfile.TemporaryDirectory() as tmp:
        for k, i in enumerate(range(int(t0 * 30), int(t1 * 30), 2)):
            shot(f, i / 30).save(os.path.join(tmp, "%04d.png" % k))
        pal = os.path.join(tmp, "pal.png")
        src = ["-framerate", "15", "-i", os.path.join(tmp, "%04d.png")]
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *src, "-vf", "palettegen=max_colors=64:stats_mode=full", pal], check=True)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *src, "-i", pal, "-lavfi", "paletteuse=dither=none", "-loop", "0",
                        os.path.join(DEST, name)], check=True)


def main():
    f = frames(sys.argv[1])
    os.makedirs(DEST, exist_ok=True)
    for t, name in [(6, "record"), (36, "layer"), (59.4, "mute"), (68, "scenes"), (76, "resample"), (92.5, "export")]:
        shot(f, t).save(os.path.join(DEST, name + ".png"), optimize=True)
    annotated(f)
    keys()
    gif(f, 39, 49, "demo.gif")


if __name__ == "__main__":
    main()
