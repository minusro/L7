#!/usr/bin/env python3
"""The install page's link-preview image (Discord, Slack, X, iMessage, ...):
the L7 logo in the page's accent green on its background, with its faint
horizontal grid lines. 1200 x 630, the size those previews expect.

    python3 tools/web_flasher/make_social_card.py   # writes tools/web_flasher/social-card.png
"""
from pathlib import Path
from PIL import Image, ImageDraw

W, H = 1200, 630
SS = 4                                   # drawn 4x and scaled down, for smooth edges
BG = (0x14, 0x16, 0x14)                  # the page's --bg
ACCENT = (0xB4, 0xE2, 0xC5)              # the page's --accent
GRID = (213, 210, 213)                   # the page's --grid colour...
GRID_ALPHA = 0.05                        # ...and opacity
GRID_STEP = 12                           # one line every 12 px, as on the page

# The logo (docs/readme/l7-logo.svg), all straight-edged polygons in a 55 x 26 box
LOGO = [
    [(8.667, 0), (15.67, 0), (10.33, 16), (20.33, 16), (19.67, 18), (2.667, 18)],
    [(55, 0), (37, 18), (27, 18), (38, 7), (27.33, 7), (29.67, 0)],
    [(2, 20), (19, 20), (18.33, 22), (1.333, 22)],
    [(35, 20), (33, 22), (23, 22), (25, 20)],
    [(0.6667, 24), (17.67, 24), (17, 26), (0, 26)],
    [(31, 24), (29, 26), (19, 26), (21, 24)],
]
LOGO_W, LOGO_H = 55, 26
LOGO_WIDTH = 640                         # px in the card, centred


def main():
    img = Image.new('RGB', (W * SS, H * SS), BG)
    d = ImageDraw.Draw(img)
    line = tuple(round(b + (g - b) * GRID_ALPHA) for b, g in zip(BG, GRID))
    for y in range(0, H, GRID_STEP):
        d.rectangle([0, y * SS, W * SS, (y + 1) * SS - 1], fill=line)
    scale = LOGO_WIDTH / LOGO_W
    x0 = (W - LOGO_WIDTH) / 2
    y0 = (H - LOGO_H * scale) / 2
    for poly in LOGO:
        d.polygon([((x0 + x * scale) * SS, (y0 + y * scale) * SS) for x, y in poly], fill=ACCENT)
    out = Path(__file__).with_name('social-card.png')
    img.resize((W, H), Image.LANCZOS).save(out, optimize=True)
    print(out)


if __name__ == '__main__':
    main()
