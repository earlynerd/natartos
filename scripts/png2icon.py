#!/usr/bin/env python3
"""Convert a PNG to a natartos NC01 icon (.rgba) for MoreMenu item f[5].

Output format (consumed by mods/menu/mod.c load_rgba_file):
  'N','C','0','1' | u32 LE width | u32 LE height | width*height*4 bytes RGBA,
  premultiplied alpha, byte order R,G,B,A.

Run in a container (no PIL on the NAS):
  docker run --rm -v "$PWD:/w" -w /w python:3.11-slim bash -c \
    "pip -q install pillow && python3 -I scripts/png2icon.py in.png out.rgba [W H]"

If W H are given the image is resized (nearest for crisp small icons; use the
stock icon size, ~64x48). Device path: /data/local/tmp/natartos_icon_<id>.rgba
"""
import sys, struct

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    from PIL import Image
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGBA")
    if len(sys.argv) >= 5:
        w, h = int(sys.argv[3]), int(sys.argv[4])
        im = im.resize((w, h), Image.LANCZOS)
    w, h = im.size
    if not (1 <= w <= 256 and 1 <= h <= 256):
        print("size %dx%d out of range (1..256)" % (w, h)); sys.exit(1)
    out = bytearray(b"NC01")
    out += struct.pack("<II", w, h)
    for (r, g, b, a) in im.getdata():
        # premultiply
        out += bytes((r * a // 255, g * a // 255, b * a // 255, a))
    open(dst, "wb").write(out)
    print("wrote %s  %dx%d  %d bytes" % (dst, w, h, len(out)))

if __name__ == "__main__":
    main()
