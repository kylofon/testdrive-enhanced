"""Writes app.ico: the chequered flag drawn in icon.cpp, for Explorer. Needs Pillow.

    python make_icon.py
"""
from pathlib import Path

from PIL import Image

tile = Image.new("RGBA", (16, 16), (0, 0, 0, 0))


def put(x, y, v):
    tile.putpixel((x, y), (v, v, v, 255))


for y in range(16):  # the pole
    put(2, y, 0x60)
    put(3, y, 0x90)
for y in range(1, 11):  # the flag: a dark border round 5 x 4 squares of 2 x 2 pixels
    for x in range(4, 16):
        border = y in (1, 10) or x in (4, 15)
        dark = border or ((x - 5) // 2 + (y - 2) // 2) % 2 == 0
        put(x, y, 0x10 if dark else 0xFF)

sizes = [16, 20, 24, 32, 48, 64, 256]
big = tile.resize((256, 256), Image.NEAREST)
big.save(Path(__file__).with_name("app.ico"), sizes=[(s, s) for s in sizes])
