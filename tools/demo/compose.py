#!/usr/bin/env python3
"""Composes the L7 demo video from what demo.cpp wrote, portrait for
Instagram Stories (1080x1920): the M8's screen (raw RGB565, 30 frames a
second) across the whole width near the top (60 px into the Stories' top
safe zone of 250 px, still clear of the profile's row), the caption and the
M8's keys (lit while held, twice their old size, reaching down into the bottom
340 px) beneath it; the app's output as the sound.

  compose.py <demo-out-dir> <video.mp4>                (needs ffmpeg, numpy, Pillow)
  compose.py <demo-out-dir> --stills 5,12.5,30 <dir>   (PNGs of those moments, to check)
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H, FPS = 1080, 1920, 30
SAFE_TOP, SAFE_BOTTOM, MARGIN = 250, 340, 60       # (Stories: the profile's bar on top, the reply bar below)
SW, SH = 320, 240
SY, SCREEN_H = SAFE_TOP - 60, W * SH // SW          # the screen's place: the whole width, 3.375x
PANEL_Y = SY + SCREEN_H                             # the caption and the keys from here down
BG = (12, 12, 14)
INK = (236, 236, 236)
DIM = (150, 150, 156)
ACCENT = (0xB5, 0xE3, 0xC4)                         # (project A's colour, as the app's)
KEY_UP, KEY_FILL = (52, 52, 58), (24, 24, 28)

EDIT, OPT, RIGHT, PLAY, SHIFT, DOWN, UP, LEFT = (1 << i for i in range(8))

FONT = "/System/Library/Fonts/HelveticaNeue.ttc"


def font(size, bold=False):
    try:
        return ImageFont.truetype(FONT, size, index=1 if bold else 0)
    except OSError:
        return ImageFont.load_default()


TITLE, SUB = font(84, True), font(50)
TITLE_Y, TITLE_STEP, SUB_GAP, SUB_STEP = 40, 92, 6, 60   # (the caption's lines, from the screen's bottom)
CAPTION_H = TITLE_Y + TITLE_STEP + SUB_GAP + 2 * SUB_STEP + 4   # (a title line, two of subtitle)


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


def area_map(n_src, n_out):
    """Scaling by area (n_out / n_src under 1 source pixel an output pixel):
    each output pixel's two source pixels and the first one's share."""
    s = n_out / n_src
    x0 = np.arange(n_out) / s
    a = np.floor(x0).astype(np.intp)
    return a, np.minimum(a + 1, n_src - 1), np.clip((a + 1 - x0) * s, 0, 1).astype(np.float32)


YA, YB, YW = area_map(SH, SCREEN_H)
XA, XB, XW = area_map(SW, W)


def scale_screen(rgb):
    """The screen up to the video's width, its pixels even (blended at their edges, not 3 or 4 wide)."""
    f = rgb.astype(np.float32)
    f = f[YA] * YW[:, None, None] + f[YB] * (1 - YW)[:, None, None]
    f = f[:, XA] * XW[None, :, None] + f[:, XB] * (1 - XW)[None, :, None]
    return (f + 0.5).astype(np.uint8)


# The keys under the caption, laid out as on the M8 (its face, in key pitches
# from the top left key's corner): OPTION (B) and EDIT (A) top right, UP a
# little lower, LEFT / DOWN / RIGHT under it, SHIFT / START below those. No
# labels: the keys' places say which they are.
PITCH = 192
KEY = int(PITCH * 0.88)
LAYOUT = {OPT: (2.18, 0), EDIT: (3.16, 0), UP: (1, 0.225), LEFT: (0, 1.17), DOWN: (1, 1.17), RIGHT: (2, 1.17),
          SHIFT: (1, 2.32), PLAY: (2, 2.32)}
KEYS_W, KEYS_H = int(3.16 * PITCH) + KEY, int(2.32 * PITCH) + KEY
KEYS_Y = PANEL_Y + CAPTION_H                        # (under a caption of three lines)


def key_boxes():
    x0, y0 = (W - KEYS_W) // 2, KEYS_Y - PANEL_Y
    return {bit: (int(x0 + c * PITCH), int(y0 + r * PITCH)) for bit, (c, r) in LAYOUT.items()}


def panel(caption, sub, keys, alpha):
    """The picture under the screen: the caption faded in by alpha (0-1), the keys."""
    img = Image.new("RGB", (W, H - PANEL_Y), BG)
    d = ImageDraw.Draw(img)
    mix = lambda c: tuple(int(BG[i] + (c[i] - BG[i]) * alpha) for i in range(3))
    y = TITLE_Y
    for line in wrap(d, caption, TITLE, W - 2 * MARGIN):
        d.text((MARGIN, y), line, font=TITLE, fill=mix(ACCENT if caption == "L7" else INK))
        y += TITLE_STEP
    y += SUB_GAP
    for line in wrap(d, sub, SUB, W - 2 * MARGIN):
        d.text((MARGIN, y), line, font=SUB, fill=mix(DIM if caption != "L7" else INK))
        y += SUB_STEP
    for bit, (x, ky) in key_boxes().items():
        down = keys & bit
        d.rounded_rectangle((x, ky, x + KEY, ky + KEY), radius=26, fill=ACCENT if down else KEY_FILL,
                            outline=ACCENT if down else KEY_UP, width=5)
    return img


def frames(out):
    meta = open(os.path.join(out, "frames.txt")).read().splitlines()
    raw = open(os.path.join(out, "frames.raw"), "rb")
    for line in meta:
        keys, caption, sub = (line.split("\t") + ["", ""])[:3]
        px = np.frombuffer(raw.read(SW * SH * 2), dtype="<u2").reshape(SH, SW)
        rgb = np.stack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31], -1).astype(np.uint8)
        yield int(keys), caption, sub, rgb


XF = 30                                             # (the end: the splash, backwards, fading in over this many frames)
CREDITS_Y = 150                                     # (the splash's credits line, under the logo, from here down: left out)


def splash_count(out):
    try:
        return int(open(os.path.join(out, "splash.txt")).read().split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def compose(out):
    """The video's frames: the demo's, then its splash played backwards (the
    app's own animation, as it started, without its credits line): its first
    frame fading in over the demo's last XF, then the rest."""
    bg = np.full((H, W, 3), BG, np.uint8)
    cache = {}
    scaled = [None, None]                           # (the last screen and its scaling: most frames repeat it)
    last, since = None, 0
    n_splash = splash_count(out)
    splash, held = [], []                           # (the splash's screens; the frames held back for the fade)
    panel_now = None

    def frame_of(rgb, pan):
        f = bg.copy()
        if scaled[0] is None or not np.array_equal(scaled[0], rgb):
            scaled[:] = rgb, scale_screen(rgb)
        f[SY:SY + SCREEN_H] = scaled[1]
        f[PANEL_Y:, :] = pan
        return f

    for i, (keys, caption, sub, rgb) in enumerate(frames(out)):
        if i < n_splash:
            rgb = rgb.copy()
            rgb[CREDITS_Y:] = rgb[SH - 1, 0]
            splash.append(rgb)
        if (caption, sub) != last:
            last, since = (caption, sub), 0
        since += 1
        alpha = min(1.0, since / 6.0)               # (a caption fades in over 0.2 s)
        k = (caption, sub, keys, round(alpha, 2))
        if k not in cache:
            if len(cache) > 400:
                cache.clear()
            cache[k] = np.array(panel(caption, sub, keys, alpha))
        panel_now = cache[(caption, sub, keys, round(alpha, 2))]
        held.append(frame_of(rgb, panel_now))
        if len(held) > XF:
            yield held.pop(0)
    end_panel = np.array(panel(last[0], last[1], 0, 1.0)) if last else panel_now
    backwards = splash[::-1]
    if not backwards:
        yield from held
        return
    first = frame_of(backwards[0], end_panel).astype(np.float32)
    for i, h in enumerate(held):                    # (fading in, eased, over the last frames)
        a = (i + 1) / (len(held) + 1)
        a = a * a * (3 - 2 * a)
        yield (h.astype(np.float32) * (1 - a) + first * a).astype(np.uint8)
    for rgb in backwards[1:]:
        yield frame_of(rgb, end_panel)


def total_frames(out):
    n = len(open(os.path.join(out, "frames.txt")).read().splitlines())
    s = splash_count(out)
    return n + s - 1 if s else n


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
    dur = total_frames(out) / FPS
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", str(FPS),
           "-i", "-", "-i", os.path.join(out, "audio.wav"),
           "-vf", "fade=t=in:st=0:d=0.4,fade=t=out:st=%.2f:d=0.6" % (dur - 0.6),
           "-af", "apad,loudnorm=I=-15:TP=-1.0:LRA=11,afade=t=out:st=%.2f:d=1.2" % (dur - 1.2),
           "-c:v", "libx264", "-preset", "slow", "-crf", "17", "-pix_fmt", "yuv420p",
           "-c:a", "aac", "-b:a", "256k", "-t", "%.3f" % dur, "-movflags", "+faststart", video]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in compose(out):
        p.stdin.write(f.tobytes())
    p.stdin.close()
    sys.exit(p.wait())


if __name__ == "__main__":
    main()
