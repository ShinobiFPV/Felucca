#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
"""The Genesynth power-on splash (240 x 240): rendered here once, written as firmware/src/genesynth_splash.h (a palette
of RGB565 colours and the image run-length coded over it, ui_draw.c draw_splash decodes it a row at a time) and a
preview PNG. The header is checked in: the build needs neither Pillow nor the font.

  tools/shintech/gen_splash.py [PREVIEW.png]        (needs Pillow and DejaVu Sans Bold, Bitstream Vera licence)

The look: a 16-bit console's boot screen. Black, blue scanline bands, the name in wide italic chrome (white sky, a
hard horizon, blue below, a pale reflection at the foot) with a dark outline, "16-BIT" in red under it, and the
credits small at the bottom: this firmware is ShinTech's fork of Felucca by Hügelton Instruments (GPL-3.0)."""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

W = H = 240
SS = 4                                          # supersampling, then down to 240
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "firmware", "src", "genesynth_splash.h")
MAXCOL = 64


def text_mask(txt, font, size, stretch, shear, track=0):
    """the text's coverage, wide (stretch x) and leaning right (shear), cropped to its ink"""
    f = ImageFont.truetype(font, size * SS)
    l, t, r, b = f.getbbox(txt)
    w = r - l + track * SS * (len(txt) - 1) + 8 * SS
    m = Image.new("L", (w, b - t + 8 * SS), 0)
    d = ImageDraw.Draw(m)
    x = 4 * SS
    for ch in txt:
        d.text((x - l, 4 * SS - t), ch, font=f, fill=255)
        x += f.getlength(ch) + track * SS
    m = m.resize((int(m.width * stretch), m.height), Image.LANCZOS)
    sh = int(m.height * shear)
    m = m.transform((m.width + sh, m.height), Image.AFFINE, (1, shear, -sh, 0, 1, 0), Image.BICUBIC)
    return m.crop(m.getbbox())


def vgrad(w, h, stops):
    """a vertical gradient over (position 0..1, (r, g, b)) stops, hard where two stops share a position"""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        p = y / max(1, h - 1)
        for i in range(len(stops) - 1):
            p0, c0 = stops[i]
            p1, c1 = stops[i + 1]
            if p0 <= p <= p1:
                k = 0 if p1 == p0 else (p - p0) / (p1 - p0)
                c = tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(3))
                break
        for x in range(w):
            px[x, y] = c
    return img


def paste_logo(canvas, mask, cx, top, fill, outline, ow):
    """mask filled with fill (an image its size), a solid outline ow px (at SS) around it, centred on cx"""
    x = cx - mask.width // 2
    grown = mask.filter(ImageFilter.MaxFilter(ow * 2 + 1))
    canvas.paste(Image.new("RGB", grown.size, outline), (x, top), grown)
    canvas.paste(fill, (x, top), mask)
    return top + mask.height


def render():
    img = Image.new("RGB", (W * SS, H * SS), (0, 0, 0))
    d = ImageDraw.Draw(img)
    # the scanline bands: thin blue lines, closer toward the logo, as a console intro's horizon
    for i, (y, h, c) in enumerate([(30, 2, (0, 40, 140)), (38, 2, (0, 60, 180)), (44, 3, (0, 90, 220)),
                                   (49, 3, (20, 120, 255)),
                                   (171, 3, (20, 120, 255)), (177, 3, (0, 90, 220)), (183, 2, (0, 60, 180)),
                                   (191, 2, (0, 40, 140))]):
        d.rectangle([0, y * SS, W * SS, (y + h) * SS - 1], fill=c)
    # the name: wide italic chrome
    name = text_mask("GENESYNTH", BOLD, 30, 1.18, 0.22, track=-1)
    if name.width > 228 * SS:
        name = name.resize((228 * SS, int(name.height * 228 * SS / name.width)), Image.LANCZOS)
    chrome = vgrad(name.width, name.height, [(0.0, (255, 255, 255)), (0.30, (190, 225, 255)),
                                             (0.50, (120, 185, 255)), (0.50, (10, 40, 150)),
                                             (0.72, (30, 90, 220)), (1.0, (200, 235, 255))])
    y = 70 * SS
    y = paste_logo(img, name, W * SS // 2, y, chrome, (0, 10, 60), 2 * SS)
    # a glint: a white bar along the horizon, only inside the letters
    glint = Image.new("L", name.size, 0)
    ImageDraw.Draw(glint).rectangle([0, int(name.height * 0.50) - SS, name.width, int(name.height * 0.50)], fill=255)
    glint = ImageChops.multiply(glint, name)
    img.paste(Image.new("RGB", name.size, (255, 255, 255)), (W * SS // 2 - name.width // 2, 70 * SS), glint)
    # 16-BIT in red, under it
    bit = text_mask("16-BIT", BOLD, 22, 1.25, 0.22, track=1)
    red = vgrad(bit.width, bit.height, [(0.0, (255, 120, 90)), (0.45, (240, 30, 20)), (1.0, (150, 0, 0))])
    paste_logo(img, bit, W * SS // 2, y + 14 * SS, red, (40, 0, 0), SS + SS // 2)
    # the credits
    f = ImageFont.truetype(SANS, 9 * SS)
    fb = ImageFont.truetype(BOLD, 9 * SS)
    for txt, yy, font, c in [("SHINTECH ELECTRONICS", 205, fb, (200, 200, 200)),
                             ("based on Felucca · Hügelton Instruments", 218, f, (120, 120, 140))]:
        w = d.textlength(txt, font=font)
        d.text(((W * SS - w) / 2, yy * SS), txt, font=font, fill=c)
    return img.resize((W, H), Image.LANCZOS)


def quantize(img):
    q = img.quantize(colors=MAXCOL, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:MAXCOL * 3]
    return q, [tuple(pal[i * 3:i * 3 + 3]) for i in range(MAXCOL)]


def rgb565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rle(q):
    """per row, runs in two forms: a byte b < 0xC0 is index b & 63, (b >> 6) + 1 times (1..3); 0xC0 | n then a
    byte i is index i, n + 4 times (4..67). A run never crosses a row: each row ends at exactly 240 px."""
    px = q.load()
    out = []
    for y in range(H):
        x = 0
        while x < W:
            i = px[x, y]
            n = 1
            while x + n < W and px[x + n, y] == i and n < 67:
                n += 1
            if n <= 3:
                out.append(((n - 1) << 6) | i)
            else:
                out += [0xC0 | (n - 4), i]
            x += n
    return out


def main():
    img = render()
    q, pal = quantize(img)
    data = rle(q)
    used = max(q.getdata()) + 1
    with open(OUT, "w", newline="\n") as f:
        f.write("/* SPDX-License-Identifier: GPL-3.0-only\n * Copyright (C) 2026 William Kew, ShinTech Electronics */\n")
        f.write("/* GENERATED by tools/shintech/gen_splash.py: the Genesynth power-on splash, 240 x 240 (ui_draw.c "
                "draw_splash).\n * GS_PAL: RGB565. GS_RLE, row after row, each row exactly 240 px: a byte b < 0xC0 is "
                "index b & 63, (b >> 6) + 1\n * times; 0xC0 | n then a byte i is index i, n + 4 times */\n")
        f.write("#define GS_NPAL %du\n" % used)
        f.write("static const uint16_t GS_PAL[GS_NPAL] = {\n")
        for i in range(0, used, 8):
            f.write("    " + ", ".join("0x%04X" % rgb565(c) for c in pal[i:i + 8]) + ",\n")
        f.write("};\n#define GS_RLE_LEN %du\n" % len(data))
        f.write("static const uint8_t GS_RLE[GS_RLE_LEN] = {\n")
        for i in range(0, len(data), 16):
            f.write("    " + ", ".join("%d" % b for b in data[i:i + 16]) + ",\n")
        f.write("};\n")
    prev = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "shintech", "splash.png")
    os.makedirs(os.path.dirname(prev), exist_ok=True)
    q.convert("RGB").resize((W * 2, H * 2), Image.NEAREST).save(prev)
    print(f"gen_splash: {OUT}: {used} colours, {len(data)} bytes; preview {prev}")


if __name__ == "__main__":
    main()
