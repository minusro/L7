#!/usr/bin/env python3
"""Composes the L7 demo video from what demo.cpp wrote: the M8's screen (raw
RGB565, 30 frames a second) scaled up 4x on the left, the caption and the M8's
keys (lit while held) on the right, the app's own output as the sound.

  compose.py <demo-out-dir> <video.mp4>                (needs ffmpeg, numpy, Pillow)
  compose.py <demo-out-dir> --stills 5,12.5,30 <dir>   (PNGs of those moments, to check)
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H, FPS = 1920, 1080, 30
SW, SH, SCALE = 320, 240, 4
SX, SY = 72, (H - SH * SCALE) // 2                  # the screen's place
PANEL_X = SX + SW * SCALE + 72                      # the panel's left edge
BG = (12, 12, 14)
INK = (236, 236, 236)
DIM = (128, 128, 134)
ACCENT = (0xB5, 0xE3, 0xC4)                         # (project A's colour, as the app's)
KEY_UP, KEY_FILL = (44, 44, 50), (24, 24, 28)

EDIT, OPT, RIGHT, PLAY, SHIFT, DOWN, UP, LEFT = (1 << i for i in range(8))

FONT = "/System/Library/Fonts/HelveticaNeue.ttc"


def font(size, bold=False):
    try:
        return ImageFont.truetype(FONT, size, index=1 if bold else 0)
    except OSError:
        return ImageFont.load_default()


TITLE, SUB, KEYF, SMALL = font(76, True), font(36), font(26, True), font(21)


def wrap(draw, text, f, width):
    lines, line = [], ""
    for word in text.split():
        trial = (line + " " + word).strip()
        if draw.textlength(trial, font=f) <= width or not line:
            line = trial
        else:
            lines.append(line)
            line = word
    if line:
        lines.append(line)
    return lines


def base():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    bez = 18                                        # (the M8's screen bezel)
    d.rounded_rectangle((SX - bez, SY - bez, SX + SW * SCALE + bez, SY + SH * SCALE + bez), radius=22, fill=(28, 28, 32))
    d.rounded_rectangle((SX - 3, SY - 3, SX + SW * SCALE + 3, SY + SH * SCALE + 3), radius=6, fill=(0, 0, 0))
    return img


# The keys: the arrows on the left, B / A over SHIFT / START on the right.
KEY = 74
KEYS_Y = 650


def key_boxes():
    x0 = PANEL_X
    gap = 10
    arrows = {UP: (x0 + KEY + gap, KEYS_Y), LEFT: (x0, KEYS_Y + KEY + gap),
              DOWN: (x0 + KEY + gap, KEYS_Y + KEY + gap), RIGHT: (x0 + 2 * (KEY + gap), KEYS_Y + KEY + gap)}
    rx = x0 + 3 * (KEY + gap) + 34
    right = {OPT: (rx, KEYS_Y), EDIT: (rx + KEY + gap, KEYS_Y),
             SHIFT: (rx, KEYS_Y + KEY + gap), PLAY: (rx + KEY + gap, KEYS_Y + KEY + gap)}
    return {**arrows, **right}


LABELS = {UP: "▲", DOWN: "▼", LEFT: "◀", RIGHT: "▶", SHIFT: "SHIFT", PLAY: "START", OPT: "B", EDIT: "A"}
ARROW_FONT = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 30)


def panel(caption, sub, keys, alpha):
    """The panel's picture (the right side), the caption faded in by alpha (0-1)."""
    img = Image.new("RGB", (W - PANEL_X, H), BG)
    d = ImageDraw.Draw(img)
    mix = lambda c: tuple(int(BG[i] + (c[i] - BG[i]) * alpha) for i in range(3))
    y = 250
    for line in wrap(d, caption, TITLE, W - PANEL_X - 60):
        d.text((0, y), line, font=TITLE, fill=mix(ACCENT if caption == "L7" else INK))
        y += 86
    y += 14
    for line in wrap(d, sub, SUB, W - PANEL_X - 70):
        d.text((0, y), line, font=SUB, fill=mix(DIM if caption != "L7" else INK))
        y += 46
    for bit, (x, ky) in key_boxes().items():
        x -= PANEL_X
        down = keys & bit
        d.rounded_rectangle((x, ky, x + KEY, ky + KEY), radius=14, fill=ACCENT if down else KEY_FILL,
                            outline=ACCENT if down else KEY_UP, width=3)
        label = LABELS[bit]
        f = ARROW_FONT if bit in (UP, DOWN, LEFT, RIGHT) else (KEYF if len(label) == 1 else SMALL)
        tw = d.textlength(label, font=f)
        d.text((x + (KEY - tw) / 2, ky + KEY / 2), label, font=f, anchor="lm", fill=(12, 12, 14) if down else DIM)
    d.text((0, KEYS_Y + 2 * KEY + 40), "the M8's keys, as pressed", font=SMALL, fill=(70, 70, 76))
    return img


def frames(out):
    meta = open(os.path.join(out, "frames.txt")).read().splitlines()
    raw = open(os.path.join(out, "frames.raw"), "rb")
    for line in meta:
        keys, caption, sub = (line.split("\t") + ["", ""])[:3]
        px = np.frombuffer(raw.read(SW * SH * 2), dtype="<u2").reshape(SH, SW)
        rgb = np.stack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31], -1).astype(np.uint8)
        yield int(keys), caption, sub, rgb


def compose(out):
    bg = np.array(base())
    cache = {}
    last, since = None, 0
    for keys, caption, sub, rgb in frames(out):
        if (caption, sub) != last:
            last, since = (caption, sub), 0
        since += 1
        alpha = min(1.0, since / 6.0)               # (a caption fades in over 0.2 s)
        k = (caption, sub, keys, round(alpha, 2))
        if k not in cache:
            if len(cache) > 400:
                cache.clear()
            cache[k] = np.array(panel(caption, sub, keys, alpha))
        frame = bg.copy()
        frame[SY:SY + SH * SCALE, SX:SX + SW * SCALE] = rgb.repeat(SCALE, 0).repeat(SCALE, 1)
        frame[:, PANEL_X:] = cache[k]
        yield frame


def main():
    out = sys.argv[1]
    if len(sys.argv) > 3 and sys.argv[2] == "--stills":
        at = [float(s) for s in sys.argv[3].split(",")]
        dest = sys.argv[4]
        os.makedirs(dest, exist_ok=True)
        want = {int(t * FPS): t for t in at}
        for i, f in enumerate(compose(out)):
            if i in want:
                Image.fromarray(f).save(os.path.join(dest, "still_%05.1f.png" % want[i]))
            if i > max(want):
                break
        return
    video = sys.argv[2]
    n = len(open(os.path.join(out, "frames.txt")).read().splitlines())
    dur = n / FPS
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", str(FPS),
           "-i", "-", "-i", os.path.join(out, "audio.wav"),
           "-vf", "fade=t=in:st=0:d=0.4,fade=t=out:st=%.2f:d=1.2" % (dur - 1.2),
           "-af", "loudnorm=I=-15:TP=-1.0:LRA=11,afade=t=out:st=%.2f:d=1.2" % (dur - 1.2),
           "-c:v", "libx264", "-preset", "slow", "-crf", "17", "-pix_fmt", "yuv420p",
           "-c:a", "aac", "-b:a", "256k", "-t", "%.3f" % dur, "-movflags", "+faststart", video]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in compose(out):
        p.stdin.write(f.tobytes())
    p.stdin.close()
    sys.exit(p.wait())


if __name__ == "__main__":
    main()
