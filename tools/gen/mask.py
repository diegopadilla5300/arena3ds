# The logo as an 8-bit alpha mask for the tab bar: writes a C header.
# Usage: mask.py logo.png out.h width
import sys
from PIL import Image
logo = Image.open(sys.argv[1]).convert("RGBA")
w = int(sys.argv[3])
h = round(logo.height * w / logo.width)
a = logo.resize((w, h), Image.LANCZOS).getchannel("A")
px = list(a.getdata())
with open(sys.argv[2], "w") as f:
    f.write("/* Are.na's logo, %dx%d alpha, made by tools/gen/mask.py. */\n#pragma once\n" % (w, h))
    f.write("#define LOGO_W %d\n#define LOGO_H %d\n" % (w, h))
    f.write("static const unsigned char LOGO_ALPHA[%d] = {\n" % (w * h))
    for i in range(0, len(px), 24):
        f.write("\t" + ",".join(str(v) for v in px[i:i + 24]) + ",\n")
    f.write("};\n")
