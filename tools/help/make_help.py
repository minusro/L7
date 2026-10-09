#!/usr/bin/env python3
# make_help.py — the help pages (ui_help.cpp) from their content and the UI
# preview's screenshots: writes help_slides.h (the sketch folder), each page a
# list of drawing steps (rectangles, text in L7's font at 1-6 px a dot,
# images), the images run-length coded with their own palettes, all in flash.
#
#   tools/ui_preview/ui_preview <dir>     (the screenshots: render them first)
#   python3 tools/help/make_help.py <dir>
#
# The layout: the poster's (a big title top left, the page's number in a tile
# top right, numbered screenshots with captions, two lines at the bottom) in
# the menu's colours (its ground, the page's colour on the title, the tile, the
# circles, the rule and the screenshots' accent: their hue turned to it). The
# page row under it (seen pages, the current one) is drawn by ui_help.cpp.
import colorsys, os, re, sys
from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
PREV = sys.argv[1]

# ---- The pages: the title (two lines), the screenshots (preview name, caption),
# notes beside a lone screenshot, two lines of text (the first page: three).
PAGES = [
    dict(title=("L7", "HOW TO"), panels=[], lines=("A LOOPER FOR THE M8", "7 STEREO TRACKS, 200 SCENES AND 12 PROJECTS", "< > TO MOVE")),
    dict(title=("THE", "SCREEN"), panels=[("ui_wave", "7 TRACKS")],
         lines=("A TRACK: SOUND, FILTER, VOLUME", "BELOW: SCENE, TIME, TEMPO"), notes=["^v<> SELECT", "A: DO IT"]),
    dict(title=("RECORD", ""), panels=[("ui_empty", "EMPTY"), ("ui_recording", "RECORD"), ("ui_wave", "PLAY")],
         lines=("A: RECORD", "A AGAIN: STOP. IT PLAYS")),
    dict(title=("MUTE", ""), panels=[("ui_wave", "PLAY"), ("ui_muted", "MUTE")],
         lines=("A: MUTE OR PLAY", "HOLD A, PRESS ^v: MORE TRACKS")),
    dict(title=("LENGTH", ""), panels=[("ui_wave", "LONG"), ("ui_wave_edit", "SHORT")],
         lines=("HOLD B, PRESS ^v: LONGER OR SHORTER", "HOLD B, PRESS < >: MOVE IT")),
    dict(title=("FILTER", "VOLUME"), panels=[("ui_filter_edit", "FILTER"), ("ui_mix_edit", "VOLUME")],
         lines=("HOLD B ON A FILTER OR VOLUME", "PRESS THE ARROWS TO CHANGE IT")),
    dict(title=("CLIPS", ""), panels=[("ui_cut", "CUT"), ("ui_paste", "PASTE"), ("ui_shift_track", "CHANGE")],
         lines=("SHIFT+A: CUT OR PASTE", "SHIFT+< >: OTHER SOUNDS")),
    dict(title=("SCENES", ""), panels=[("ui_wave", "SCENE 1"), ("ui_status_scene_edit", "SCENE 2")],
         lines=("A SCENE KEEPS ALL TRACKS", "SHIFT+^v: NEXT SCENE")),
    dict(title=("TEMPO", ""), panels=[("ui_status_tap_held", "TAP"), ("ui_status_metro_on", "CLICK")],
         lines=("START: PLAY OR STOP", "A ON TEMPO: TAP. B+A: CLICK")),
    dict(title=("RESAMPLE", "AND MOVE"), panels=[("ui_resampling", "RESAMPLE"), ("ui_move", "MOVE")],
         lines=("B+A: RECORD WHAT YOU HEAR", "B+^v: MOVE A TRACK")),
    dict(title=("SYNC", ""), panels=[("ui_status_follow_on", "FOLLOW")],
         lines=("L7 FOLLOWS MIDI AND USB CLOCK", "SHIFT+A ON TEMPO: FOLLOW AUDIO"), notes=["MIDI", "USB", "AUDIO"]),
    dict(title=("PROJECTS", ""), panels=[("ui_menu_load", "OPEN"), ("ui_menu_cut", "CUT")],
         lines=("ARROW OFF THE EDGE: MENU", "A: OPEN. SHIFT+A: CUT")),
]

# ---- L7's font (its widths: centring, wrapping)
src = open(os.path.join(ROOT, "ui_font.cpp")).read()
GLYPHS = {}
for m in re.finditer(r"\{'(.)',\s*\{((?:\"[.#]*\",?\s*)+)\}", src):
    GLYPHS[m.group(1)] = re.findall(r'"([.#]*)"', m.group(2))
def g(ch): return GLYPHS.get(ch) or GLYPHS.get(ch.upper()) or GLYPHS[' ']
def text_w(s, k=1): return max(0, sum((len(g(c)[0]) + 1) * k for c in s) - k)
for line in [l for p in PAGES for l in p["lines"] + tuple(p.get("notes", [])) + p["title"]] + [c for p in PAGES for _, c in p["panels"]]:
    for ch in line:
        if ch not in GLYPHS and ch.upper() not in GLYPHS: sys.exit(f"not in the font: {ch!r} ({line})")

def rgb(h): return ((h >> 16) & 255, (h >> 8) & 255, h & 255)
def ev(v): return v // 2 * 2                         # (on the 2 px grid)
PROJ = [rgb(h) for h in (0xB5E3C4, 0xD6EBA8, 0xF7EDA3, 0xFAD9A1, 0xF8C8A8, 0xF4B6C2,
                         0xF2B8D8, 0xE3B5E6, 0xCDB8EE, 0xB7C4F0, 0xA9D6EE, 0xA8E0D8)]
def c565(c): return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)
# The UI's colours, by name in the code (colors.h): exact on the device.
NAMED = {"BG": (0, 0, 0), "DIM": (112, 112, 112), "LIGHT": (207, 207, 207), "INK": (255, 255, 255), "MENU_BG": (43, 45, 42)}

# ---- Screenshots: at the screen's size, their accent turned to the page's hue
_shots = {}
def shot(name, acc):
    k = (name, acc)
    if k not in _shots:
        im = Image.open(os.path.join(PREV, name + ".png")).convert("RGB").resize((320, 240), Image.NEAREST)
        h = colorsys.rgb_to_hls(*(v / 255 for v in acc))[0]
        px = im.load()
        for y in range(240):
            for x in range(320):
                r, gg, b = px[x, y]
                _, l, sat = colorsys.rgb_to_hls(r / 255, gg / 255, b / 255)
                if sat > 0.15 and max(r, gg, b) - min(r, gg, b) > 12:
                    px[x, y] = tuple(round(v * 255) for v in colorsys.hls_to_rgb(h, l, sat))
        _shots[k] = im
    return _shots[k]

# ---- Images: a palette (RGB565, up to 255; index 255 transparent) and runs
# (each: its length less one, 0-255, then its index), row by row.
images, image_keys = [], {}
def image(key, im):
    if key in image_keys: return image_keys[key]
    w, h = im.size
    px = im.load()
    pal, runs = [], []
    for y in range(h):
        x = 0
        while x < w:
            p = px[x, y]
            n = 1
            while x + n < w and px[x + n, y] == p and n < 256: n += 1
            if len(p) == 4 and p[3] < 128: idx = 255
            else:
                c = c565(p[:3])
                if c not in pal: pal.append(c)
                idx = pal.index(c)
            runs += [n - 1, idx]
            x += n
    if len(pal) > 255: sys.exit(f"{key}: {len(pal)} colours")
    name = f"HELP_IMG_{len(images)}"
    images.append((name, w, h, pal, runs))
    image_keys[key] = name
    return name

# The numbered circle, blocky (on the 2 px grid): 11 x 11 blocks, the page's
# colour, a black ring a block wide.
def circle(acc):
    im = Image.new("RGBA", (22, 22), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    inside = lambda r, c: ((r - 5) ** 2 + (c - 5) ** 2) ** 0.5 <= 5.4
    for r in range(11):
        for c in range(11):
            if not inside(r, c): continue
            ring = any(not inside(r + dr, c + dc) for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            d.rectangle([c * 2, r * 2, c * 2 + 1, r * 2 + 1], fill=(0, 0, 0, 255) if ring else acc + (255,))
    return image(("circle", acc), im)

# ---- A page's steps: ("rect", x, y, w, h, colour) / ("text", x, y, k, colour, text) / ("image", x, y, name)
def page(n, p):
    acc = PROJ[n - 1]
    ops = [("rect", 0, 0, 320, 240, NAMED["MENU_BG"])]
    t1, t2 = p["title"]
    if n == 1:
        # The logo as the menu's (drawn from its shape, not an image), twice its size: 110 x 52, on the ground.
        ops.append(("logo", 160 - 110 // 2 // 2 * 2, 36, 2, acc))
        ops.append(("text", ev(160 - text_w("HOW TO", 4) // 2), 104, 4, acc, "HOW TO"))
        lines = []
        for line, ink in zip(p["lines"], ("LIGHT", "LIGHT", "DIM")):
            if text_w(line, 2) > 304 and " AND " in line:      # (a natural break: before its last "and")
                a, b = line.rsplit(" AND ", 1)
                lines += [(a, ink), ("AND " + b, ink)]
            else:
                lines.append((line, ink))
        for j, (line, ink) in enumerate(lines):
            ops.append(("text", ev(160 - text_w(line, 2) // 2), 140 + j * 14, 2, NAMED[ink], line))
        return ops, 80
    ops.append(("text", 8, 8, 4, acc, t1))
    if t2: ops.append(("text", 8, 32, 4, NAMED["INK"], t2))
    ops.append(("rect", 254, 6, 60, 48, acc))
    ops.append(("text", ev(284 - text_w(str(n), 6) // 2), 16, 6, NAMED["BG"], str(n)))
    count = len(p["panels"])
    places = {1: [(8, 64, 144, 108)], 2: [(12, 66, 128, 96), (180, 66, 128, 96)], 3: [(8 + i * 104, 68, 96, 72) for i in range(3)]}[count]
    for i, ((name, lab), (x, y, w, h)) in enumerate(zip(p["panels"], places)):
        ops.append(("rect", x - 2, y - 2, w + 4, h + 4, NAMED["BG"]))   # (a black frame a block wide)
        ops.append(("image", x, y, image((name, acc, w, h), shot(name, acc).resize((w, h), Image.NEAREST))))
        if count > 1:
            cx = ev(max(x - 10, 2))                           # (on the screen: the first one in from its edge)
            ops.append(("image", cx, ev(y - 12), circle(acc)))
            ops.append(("text", cx + 8, ev(y - 12) + 6, 2, NAMED["BG"], str(i + 1)))   # (its digit: 4 blocks in, 3 down)
            ops.append(("text", x, y + h + 6, 2, NAMED["LIGHT"], lab))
    if p.get("notes"):
        ops.append(("text", 164, 66, 2, NAMED["INK"], p["panels"][0][1]))
        for j, line in enumerate(p["notes"]):
            ops.append(("text", 164, 86 + j * 14, 2, acc if j == 0 else NAMED["LIGHT"], line))
    ops.append(("rect", 0, 182, 320, 2, acc))
    ops.append(("text", 8, 188, 2, NAMED["LIGHT"], p["lines"][0]))
    ops.append(("text", 8, 202, 2, NAMED["LIGHT"], p["lines"][1]))
    return ops, 10

pages = [page(i + 1, p) for i, p in enumerate(PAGES)]

# ---- help_slides.h
out = ["// help_slides.h — the help pages, generated by tools/help/make_help.py from",
       "// its content and the UI preview's screenshots: don't edit, run it again.",
       "// (Included after ui.h: PROGMEM and the types from there.)",
       "#pragma once", "",
       "struct HelpImage { uint16_t w, h; const uint16_t *pal; const uint8_t *runs; uint32_t runCount; };",
       "enum { HELP_RECT, HELP_TEXT, HELP_IMAGE, HELP_LOGO };   // (the logo: drawLogoScaled(), k its scale)",
       "struct HelpOp { uint8_t op, k; int16_t x, y, w, h; uint16_t color; const char *text; const HelpImage *img; };",
       "struct HelpPage { const HelpOp *ops; uint8_t count; int16_t dotsX; };", ""]
total = 0
for name, w, h, pal, runs in images:
    out.append(f"PROGMEM static const uint16_t {name}_PAL[] = {{{', '.join(f'0x{c:04X}' for c in pal)}}};")
    out.append(f"PROGMEM static const uint8_t {name}_RUNS[] = {{")
    for i in range(0, len(runs), 32): out.append("  " + ",".join(str(v) for v in runs[i:i + 32]) + ",")
    out.append("};")
    out.append(f"PROGMEM static const HelpImage {name} = {{{w}, {h}, {name}_PAL, {name}_RUNS, {len(runs) // 2}}};")
    total += len(runs) + 2 * len(pal)
strings = {}
def string(s):
    if s not in strings: strings[s] = f"HELP_STR_{len(strings)}"
    return strings[s]
page_ops = []
for n, (ops, dx) in enumerate(pages, 1):
    rows = []
    for op in ops:
        if op[0] == "rect": rows.append(f"{{HELP_RECT, 0, {op[1]}, {op[2]}, {op[3]}, {op[4]}, 0x{c565(op[5]):04X}, nullptr, nullptr}}")
        elif op[0] == "logo": rows.append(f"{{HELP_LOGO, {op[3]}, {op[1]}, {op[2]}, 0, 0, 0x{c565(op[4]):04X}, nullptr, nullptr}}")
        elif op[0] == "text": rows.append(f"{{HELP_TEXT, {op[3]}, {op[1]}, {op[2]}, 0, 0, 0x{c565(op[4]):04X}, {string(op[5])}, nullptr}}")
        else: rows.append(f"{{HELP_IMAGE, 0, {op[1]}, {op[2]}, 0, 0, 0, nullptr, &{op[3]}}}")
    page_ops.append((n, rows, dx))
for s, name in strings.items():
    out.append(f'PROGMEM static const char {name}[] = "{s}";')
for n, rows, dx in page_ops:
    out.append(f"PROGMEM static const HelpOp HELP_PAGE_{n}[] = {{")
    out += ["  " + r + "," for r in rows]
    out.append("};")
out.append(f"constexpr int HELP_PAGES = {len(pages)};")
out.append("PROGMEM static const HelpPage HELP[HELP_PAGES] = {")
out += [f"  {{HELP_PAGE_{n}, {len(rows)}, {dx}}}," for n, rows, dx in page_ops]
out.append("};")
open(os.path.join(ROOT, "help_slides.h"), "w").write("\n".join(out) + "\n")
print(f"help_slides.h: {len(pages)} pages, {len(images)} images, {total // 1024} KB of image data")
