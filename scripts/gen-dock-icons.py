#!/usr/bin/env python3
"""Genera los iconos de apps del Dock (BMP 32 bpp con alfa, 128x128) en
assets/system/icons/app-*.bmp. Estilo de icono "baldosa" redondeada con brillo
suave, como los de macOS. Son placeholders de diseño propio: se pueden
sustituir por arte definitivo con el mismo nombre."""
import math, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 128
SS = 4  # supersampling
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'assets', 'system', 'icons')
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf'

def squircle_mask(size, inset, n=5.0):
    m = Image.new('L', (size, size), 0)
    px = m.load()
    c = size / 2
    r = c - inset
    for y in range(size):
        for x in range(size):
            dx = abs(x + 0.5 - c) / r
            dy = abs(y + 0.5 - c) / r
            if dx ** n + dy ** n <= 1.0:
                px[x, y] = 255
    return m

def vgrad(size, top, bottom):
    g = Image.new('RGB', (size, size))
    d = ImageDraw.Draw(g)
    for y in range(size):
        t = y / (size - 1)
        d.line([(0, y), (size, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return g

def tile(top, bottom, glyph):
    N = S * SS
    img = vgrad(N, top, bottom).convert('RGBA')
    mask = squircle_mask(N, 5 * SS)
    # brillo suave en la mitad superior
    gloss = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    gd = ImageDraw.Draw(gloss)
    for y in range(N // 2):
        a = int(46 * (1 - y / (N / 2)))
        gd.line([(0, y), (N, y)], fill=(255, 255, 255, a))
    img = Image.alpha_composite(img, gloss)
    glyph(img, N)
    # contorno interior claro (1 px)
    edge = mask.filter(ImageFilter.MinFilter(2 * SS + 1))
    rim = Image.new('RGBA', (N, N), (255, 255, 255, 0))
    rim.putalpha(Image.eval(Image.composite(mask, Image.new('L', (N, N), 0), mask), lambda v: 0))
    out = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out.resize((S, S), Image.LANCZOS)

def text_center(img, N, s, size, color, dy=0):
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, int(size * SS))
    w = d.textlength(s, font=f)
    d.text(((N - w) / 2, N * 0.5 + dy * SS - size * SS * 0.62), s, font=f, fill=color)

def glyph_foot(img, N):
    text_center(img, N, '>_', 62, (255, 255, 255, 255), 4)

def glyph_wterm(img, N):
    d = ImageDraw.Draw(img)
    for i, c in enumerate(((255, 95, 87), (254, 188, 46), (40, 200, 64))):
        x = (24 + i * 13) * SS
        d.ellipse([x - 4 * SS, 26 * SS - 4 * SS, x + 4 * SS, 26 * SS + 4 * SS], fill=c + (255,))
    text_center(img, N, '$', 66, (48, 209, 88, 255), 10)

def glyph_flower(img, N):
    d = ImageDraw.Draw(img)
    cx = cy = N / 2
    for k in range(6):
        a = k * math.pi / 3
        px, py = cx + math.cos(a) * 26 * SS, cy + math.sin(a) * 26 * SS
        d.ellipse([px - 17 * SS, py - 17 * SS, px + 17 * SS, py + 17 * SS], fill=(255, 255, 255, 235))
    d.ellipse([cx - 12 * SS, cy - 12 * SS, cx + 12 * SS, cy + 12 * SS], fill=(255, 214, 10, 255))

def glyph_dot(img, N):
    d = ImageDraw.Draw(img)
    cx = cy = N / 2
    for r, w in ((40, 6), (22, 6)):
        d.ellipse([cx - r * SS, cy - r * SS, cx + r * SS, cy + r * SS], outline=(255, 255, 255, 235), width=w * SS)
    d.ellipse([cx - 9 * SS, cy - 9 * SS, cx + 9 * SS, cy + 9 * SS], fill=(255, 255, 255, 255))

def glyph_app(img, N):
    text_center(img, N, 'A', 70, (255, 255, 255, 255), 8)

def save_bmp(im, path):
    w, h = im.size
    px = im.tobytes('raw', 'BGRA')
    rows = b''.join(px[(h - 1 - y) * w * 4:(h - y) * w * 4] for y in range(h))
    hdr = b'BM' + (54 + len(rows)).to_bytes(4, 'little') + b'\0\0\0\0' + (54).to_bytes(4, 'little')
    info = ((40).to_bytes(4, 'little') + w.to_bytes(4, 'little') + h.to_bytes(4, 'little', signed=False) +
            (1).to_bytes(2, 'little') + (32).to_bytes(2, 'little') + (0).to_bytes(4, 'little') +
            len(rows).to_bytes(4, 'little') + (2835).to_bytes(4, 'little') * 2 + (0).to_bytes(8, 'little'))
    open(path, 'wb').write(hdr + info + rows)

ICONS = {
    'app-foot': ((74, 74, 78), (28, 28, 30), glyph_foot),
    'app-weston-terminal': ((60, 72, 92), (26, 34, 48), glyph_wterm),
    'app-flower': ((255, 110, 150), (214, 40, 107), glyph_flower),
    'app-clickdot': ((72, 214, 112), (26, 150, 66), glyph_dot),
    'app-generic': ((120, 170, 255), (40, 100, 230), glyph_app),
}
os.makedirs(OUT, exist_ok=True)
for name, (top, bot, g) in ICONS.items():
    im = tile(top, bot, g)
    save_bmp(im, os.path.join(OUT, name + '.bmp'))
    im.save('/tmp/' + name + '.png')
    print('OK', name)
