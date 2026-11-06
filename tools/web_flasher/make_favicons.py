#!/usr/bin/env python3
"""The install page's PNG icons, for browsers that don't take favicon.svg
(Safari before 26):

  favicon-light.png      the logo in the page's dark grey, for a light theme
  favicon-dark.png       the logo in the accent green, for a dark theme
  apple-touch-icon.png   the logo in the accent green on the page's background
                         (Safari's favourites and an iPhone or iPad home screen)

    python3 tools/web_flasher/make_favicons.py
"""
from pathlib import Path
from PIL import Image, ImageDraw
from make_social_card import LOGO, LOGO_W, LOGO_H, BG, ACCENT

SS = 4                                   # drawn 4x and scaled down, for smooth edges
HERE = Path(__file__).parent


def draw(size, colour, width, bg=None):
    img = Image.new('RGBA', (size * SS, size * SS), bg + (255,) if bg else (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    scale = width / LOGO_W
    x0, y0 = (size - width) / 2, (size - LOGO_H * scale) / 2
    for poly in LOGO:
        d.polygon([((x0 + x * scale) * SS, (y0 + y * scale) * SS) for x, y in poly], fill=colour)
    return img.resize((size, size), Image.LANCZOS)


def main():
    for name, img in [
        ('favicon-light.png', draw(64, BG, 64)),          # full width, as favicon.svg
        ('favicon-dark.png', draw(64, ACCENT, 64)),
        ('apple-touch-icon.png', draw(180, ACCENT, 132, BG).convert('RGB')),
    ]:
        out = HERE / name
        img.save(out, optimize=True)
        print(out)


if __name__ == '__main__':
    main()
