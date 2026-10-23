#!/usr/bin/env python3
"""Makes ableton_icon.h: the resource fork of the "Icon\\r" file Live puts in its project
folders (what macOS shows as the folder's icon), from a project Live saved.

  python3 tools/ableton/make_icon.py "docs/design/ableton/L7 Test 1 Project" src/ableton_icon.h
"""
import sys

project, out = sys.argv[1], sys.argv[2]
data = open(project + "/Icon\r/..namedfork/rsrc", "rb").read()
with open(out, "w") as f:
    f.write("// ableton_icon.h — made by tools/ableton/make_icon.py: the resource fork of a Live project's \"Icon\\r\" file.\n")
    f.write("PROGMEM static const uint8_t ALS_ICON[%d] = {\n" % len(data))
    for i in range(0, len(data), 24):
        f.write("    " + ",".join("0x%02X" % b for b in data[i:i + 24]) + ",\n")
    f.write("};\n")
print("icon bytes", len(data))
