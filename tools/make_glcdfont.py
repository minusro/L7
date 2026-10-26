#!/usr/bin/env python3
"""src/glcdfont5x7.h from the ILI9341_t3n library's glcdfont.c (the classic 5 x 7
font the sketch's start-up messages use): run it again if that file changes.
  python3 tools/make_glcdfont.py
"""
import re, pathlib
src = pathlib.Path.home() / "Documents/Arduino/libraries/ILI9341_t3n/src/glcdfont.c"
s = src.read_text(); body = s[s.index("{") + 1 : s.rindex("}")]
vals = [int(v, 16) if v.lower().startswith("0x") else int(v) for v in re.findall(r"0[xX][0-9a-fA-F]+|\b\d+\b", body)][:256 * 5]
out = ["// glcdfont5x7.h — the classic 5 x 7 font (Adafruit GFX's glcdfont, BSD; the",
       "// same table ILI9341_t3n ships and the UI preview borrows), five column",
       "// bytes a character, bit 0 the top row: for the start-up messages the sketch",
       "// prints under the logo, on the Model:02's own display driver (ili9488_spi).",
       "// Generated from ILI9341_t3n/src/glcdfont.c by tools/make_glcdfont.py.",
       "#pragma once", "#include <stdint.h>", "", f"static const uint8_t GLCDFONT5X7[{len(vals)}] = {{"]
for i in range(0, len(vals), 20): out.append("  " + ", ".join(f"0x{v:02X}" for v in vals[i:i+20]) + ",")
pathlib.Path(__file__).resolve().parent.parent.joinpath("src/glcdfont5x7.h").write_text("\n".join(out + ["};", ""]))
print(f"src/glcdfont5x7.h: {len(vals)} bytes")
