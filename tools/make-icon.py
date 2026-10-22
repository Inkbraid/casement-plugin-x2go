#!/usr/bin/env python3
"""Draws icon.png: a desktop window with two bands crossing on its screen.

    python3 tools/make-icon.py [output]

Drawn at four times the size and scaled down, for smooth edges. Needs PIL.
"""
import sys

from PIL import Image, ImageDraw

SIZE = 256
SCALE = 4


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "icon.png"
    s = SIZE * SCALE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def box(x0, y0, x1, y1):
        return [round(v * SCALE) for v in (x0, y0, x1, y1)]

    # The window: frame, title bar with three buttons, screen.
    d.rounded_rectangle(box(14, 30, 242, 226), radius=22 * SCALE, fill=(38, 50, 64, 255))
    d.rounded_rectangle(box(14, 30, 242, 66), radius=22 * SCALE, fill=(56, 74, 94, 255))
    d.rectangle(box(14, 50, 242, 66), fill=(56, 74, 94, 255))
    for i in range(3):
        cx, cy, r = 38 + i * 22, 48, 7
        d.ellipse(box(cx - r, cy - r, cx + r, cy + r), fill=(150, 168, 188, 255))
    d.rounded_rectangle(box(28, 76, 228, 212), radius=10 * SCALE, fill=(44, 104, 158, 255))

    # Two bands crossing on the screen, the second over the first with a
    # margin of screen colour around it.
    width = 26
    a = [(62, 196), (194, 92)]
    b = [(62, 92), (194, 196)]
    d.line([tuple(round(v * SCALE) for v in p) for p in a], fill=(43, 181, 160, 255), width=width * SCALE)
    for p in a:
        r = width / 2
        d.ellipse(box(p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=(43, 181, 160, 255))
    d.line([tuple(round(v * SCALE) for v in p) for p in b], fill=(44, 104, 158, 255), width=(width + 14) * SCALE)
    d.line([tuple(round(v * SCALE) for v in p) for p in b], fill=(242, 140, 40, 255), width=width * SCALE)
    for p in b:
        r = width / 2
        d.ellipse(box(p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=(242, 140, 40, 255))

    img.resize((SIZE, SIZE), Image.LANCZOS).save(out)


if __name__ == "__main__":
    main()
