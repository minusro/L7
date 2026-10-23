#!/usr/bin/env python3
"""The README's logo (docs/readme/logo.svg and logo-light.svg), from the shape
the app draws on its splash screen (src/ui_splash.cpp, l7Row()): "L7", italic,
the 7's right edge a 45-degree line, two cuts near the bottom. Drawn there a
pixel row at a time; here the same shape as flat polygons, so it stays sharp
at any size. Two of them: white for a dark page, and the screen's own black
for a light one (the README picks between them).

  make_logo.py [dest-dir]
"""
import os
import sys

W, H = 55, 26                  # the shape's units, as in ui_splash.cpp
STEM, FOOT, FOOT_FROM = 7, 17, H - 10         # the L: its stem's width, its foot's, the row the foot starts
TOP, TOP_FROM, DIAG = 7, 21, 10               # the 7: its top line's height, where it starts, its diagonal's width
BANDS = ((0, 18), (20, 22), (24, 26))         # between the cuts (rows 18-20 and 22-24: nothing there)
COLORS = {"logo.svg": "#ffffff",          # white (colors.h: GREY_100), for a dark page
          "logo-light.svg": "#000000"}    # the screen's own black (BG), for a light one

def off(y):                    # the italic slant: a unit right every three rows up
    return (H - y) / 3

def ell(y0, y1):               # the L between two rows, its foot wider than its stem
    top = [(off(y0), y0), (off(y0) + (FOOT if y0 >= FOOT_FROM else STEM), y0)]
    bottom = [(off(y1) + FOOT if y1 > FOOT_FROM else off(y1) + STEM, y1), (off(y1), y1)]
    step = []
    if y0 < FOOT_FROM < y1:    # the foot's shoulder
        step = [(off(FOOT_FROM) + STEM, FOOT_FROM), (off(FOOT_FROM) + FOOT, FOOT_FROM)]
    return top + step + bottom

def seven(y0, y1):             # the 7 between two rows: its top line, then its diagonal
    right = [(W - y0, y0), (W - y1, y1)]
    left = [(W - DIAG - y1, y1)]
    if y0 < TOP:               # the top line reaches further left than the diagonal under it
        left += [(W - DIAG - TOP, TOP), (TOP_FROM + off(TOP), TOP)]
    left += [(TOP_FROM + off(y0) if y0 < TOP else W - DIAG - y0, y0)]
    return right + left

def path(points):
    return "M" + " ".join(f"{x:.4g},{y:.4g}" for x, y in points) + "Z"

def svg(color):
    shapes = [path(f(y0, y1)) for y0, y1 in BANDS for f in (ell, seven)]
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
            f'width="{W * 4}" height="{H * 4}" fill="{color}" role="img" aria-label="L7">\n'
            + "".join(f'  <path d="{d}"/>\n' for d in shapes) + "</svg>\n")

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    dest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "docs", "readme")
    for name, color in COLORS.items():
        with open(os.path.join(dest, name), "w") as f:
            f.write(svg(color))
        print(os.path.normpath(os.path.join(dest, name)))
