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

def glyph_clock(img, N):
    d = ImageDraw.Draw(img)
    cx = cy = N / 2
    r = 40 * SS
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 255, 255, 245))
    for k in range(12):
        a = k * math.pi / 6
        r1, r2 = (33 if k % 3 else 29) * SS, 37 * SS
        d.line([cx + math.sin(a) * r1, cy - math.cos(a) * r1, cx + math.sin(a) * r2, cy - math.cos(a) * r2],
               fill=(60, 60, 70, 255), width=(2 if k % 3 else 3) * SS)
    d.line([cx, cy, cx + 14 * SS, cy - 4 * SS], fill=(30, 30, 40, 255), width=4 * SS)
    d.line([cx, cy, cx - 3 * SS, cy - 26 * SS], fill=(30, 30, 40, 255), width=3 * SS)
    d.ellipse([cx - 4 * SS, cy - 4 * SS, cx + 4 * SS, cy + 4 * SS], fill=(255, 69, 58, 255))

def glyph_eyes(img, N):
    d = ImageDraw.Draw(img)
    for ox in (-19, 19):
        cx, cy = N / 2 + ox * SS, N / 2
        d.ellipse([cx - 17 * SS, cy - 24 * SS, cx + 17 * SS, cy + 24 * SS], fill=(255, 255, 255, 255))
        d.ellipse([cx - 7 * SS + 3 * SS, cy - 8 * SS, cx + 7 * SS + 3 * SS, cy + 10 * SS], fill=(20, 20, 24, 255))

def glyph_hi(img, N):
    text_center(img, N, 'Hi', 58, (255, 255, 255, 255), 6)

def glyph_grid(img, N):
    d = ImageDraw.Draw(img)
    for i in range(3):
        for j in range(3):
            x, y = (30 + i * 26) * SS, (30 + j * 26) * SS
            d.rounded_rectangle([x, y, x + 20 * SS, y + 20 * SS], radius=5 * SS, fill=(255, 255, 255, 235 if (i + j) % 2 == 0 else 150))

def glyph_qt(img, N):
    text_center(img, N, 'Qt', 60, (255, 255, 255, 255), 6)

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
# Iconos de tema (hicolor 128x128, PNG) de las apps de demostracion que no
# traen icono propio: el Dock y las ventanas los encuentran por nombre (Icon= del
# .desktop o el app_id).
THEME = os.path.join(os.path.dirname(OUT), '..', '..', 'precompiled', 'usr', 'share', 'icons',
                     'hicolor', '128x128', 'apps')
THEME_ICONS = {
    'xclock': ((92, 140, 255), (36, 60, 160), glyph_clock),
    'xeyes': ((120, 120, 128), (52, 52, 58), glyph_eyes),
    'hello-gtk': ((64, 200, 190), (20, 120, 130), glyph_hi),
    'gtk3-icon-browser': ((255, 170, 70), (214, 100, 20), glyph_grid),
    'qt5-application': ((90, 220, 120), (30, 150, 70), glyph_qt),
    'qt5-gallery': ((60, 210, 160), (20, 130, 110), glyph_grid),
}
os.makedirs(THEME, exist_ok=True)
for name, (top, bot, g) in THEME_ICONS.items():
    tile(top, bot, g).save(os.path.join(THEME, name + '.png'))
    print('OK tema', name)
os.makedirs(OUT, exist_ok=True)
for name, (top, bot, g) in ICONS.items():
    im = tile(top, bot, g)
    save_bmp(im, os.path.join(OUT, name + '.bmp'))
    im.save('/tmp/' + name + '.png')
    print('OK', name)
