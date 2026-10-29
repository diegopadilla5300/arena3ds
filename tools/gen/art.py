# Draws the icon and the HOME-menu banner from Are.na's logo, white on
# black. Usage: art.py logo.png icon.png banner.png font.ttf
# (logo.png is the SVG rendered large, white on transparent.)
import sys
from PIL import Image, ImageDraw, ImageFont

def place(canvas, logo, w, cx, cy):
    h = round(logo.height * w / logo.width)
    l = logo.resize((w, h), Image.LANCZOS)
    canvas.paste(l, (round(cx - w / 2), round(cy - h / 2)), l)

def icon(logo, path):
    S = 8
    im = Image.new("RGB", (48 * S, 48 * S), "black")
    place(im, logo, 38 * S, 24 * S, 24 * S)
    im.resize((48, 48), Image.LANCZOS).save(path)

def banner(logo, path, font):
    S = 4
    im = Image.new("RGB", (256 * S, 128 * S), "black")
    place(im, logo, 96 * S, 128 * S, 50 * S)
    d = ImageDraw.Draw(im)
    # "Are.na / 3DS", the trail in grey and the place in white, as in the app.
    f = ImageFont.truetype(font, 24 * S)
    a, b = "Are.na / ", "3DS"
    w = d.textlength(a + b, font=f)
    x = (256 * S - w) / 2
    d.text((x, 84 * S), a, fill=(140, 140, 140), font=f)
    d.text((x + d.textlength(a, font=f), 84 * S), b, fill="white", font=f)
    im.resize((256, 128), Image.LANCZOS).save(path)

logo = Image.open(sys.argv[1]).convert("RGBA")
icon(logo, sys.argv[2])
banner(logo, sys.argv[3], sys.argv[4])
