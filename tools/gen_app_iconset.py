#!/usr/bin/env python3
"""Set de iconos de las apps de Aurora (propuesta).

Reglas: baldosa redondeada (squircle) de color solido propio de cada app, sin
degradados, glifo blanco. El verde aurora queda para la identidad del sistema
(logo, acentos, Monitor no); cada app lleva otro tono para que el Dock no sea
una pared verde.
Salida: assets/brand/appicons/<nombre>.png (512) y hoja de muestra; con --bmp
tambien assets/system/icons/app-<nombre>.bmp (128, 32 bpp con alfa)."""
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "assets/brand/appicons")
SYS = os.path.join(ROOT, "assets/system/icons")
S = 1024  # lienzo (se reduce a 512)
W = (255, 255, 255, 255)

PAL = {  # tono de cada app (solidos)
    "archivos": (0x3D, 0x8B, 0xFF),
    "notas": (0xFF, 0xC8, 0x2E),
    "configuracion": (0x59, 0x60, 0x6B),
    "calculadora": (0xFF, 0x8A, 0x3D),
    "monitor": (0x7C, 0x6C, 0xFF),
    "imagenes": (0xF2, 0x54, 0x5B),
    "terminal": (0x14, 0x1B, 0x19),
}


def tile(col):
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((32, 32, S - 32, S - 32), radius=232, fill=col + (255,))
    return im, d


def archivos(d):
    # carpeta: pestana + cuerpo
    d.rounded_rectangle((196, 300, 520, 420), radius=44, fill=W)
    d.rounded_rectangle((196, 360, 828, 724), radius=64, fill=W)
    d.rounded_rectangle((196, 360, 828, 440), radius=40, fill=(0xE6, 0xEF, 0xFF, 255))


def notas(d):
    # hoja con lineas y esquina doblada
    d.rounded_rectangle((250, 190, 774, 834), radius=72, fill=W)
    ink = (0xB8, 0x86, 0x00, 255)
    for i, y in enumerate((330, 440, 550, 660)):
        d.rounded_rectangle((330, y, 694 if i < 3 else 560, y + 36), radius=18, fill=ink)


def configuracion(d):
    import math
    cx = cy = S // 2
    pts = []
    n = 8
    for i in range(n):
        a0 = 2 * math.pi * i / n
        for da, r in ((-0.20, 250), (-0.14, 322), (0.14, 322), (0.20, 250)):
            a = a0 + da
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=W)
    d.ellipse((cx - 250, cy - 250, cx + 250, cy + 250), fill=W)
    d.ellipse((cx - 110, cy - 110, cx + 110, cy + 110), fill=PAL["configuracion"] + (255,))


def calculadora(d):
    c = PAL["calculadora"] + (255,)
    d.rounded_rectangle((240, 190, 784, 834), radius=76, fill=W)
    d.rounded_rectangle((290, 240, 734, 380), radius=34, fill=(0xFF, 0xE6, 0xD2, 255))
    for r in range(3):
        for k in range(3):
            x = 290 + k * 154
            y = 430 + r * 130
            d.rounded_rectangle((x, y, x + 136, y + 110), radius=30, fill=(0xE9, 0xEC, 0xF0, 255))
    d.rounded_rectangle((290 + 2 * 154, 430 + 2 * 130, 290 + 2 * 154 + 136, 430 + 2 * 130 + 110), radius=30, fill=c)


def monitor(d):
    base = 760
    vals = (0.40, 0.62, 0.50, 0.82, 0.58, 0.95)
    bw, gap = 78, 30
    x0 = (S - (len(vals) * bw + (len(vals) - 1) * gap)) // 2
    for i, v in enumerate(vals):
        h = int(520 * v)
        x = x0 + i * (bw + gap)
        d.rounded_rectangle((x, base - h, x + bw, base), radius=24, fill=W)
    d.rounded_rectangle((x0 - 6, base + 40, S - x0 + 6, base + 56), radius=8, fill=(0xD8, 0xD3, 0xFF, 255))


def imagenes(d):
    # marco con sol y dos montanas
    d.rounded_rectangle((196, 240, 828, 784), radius=70, fill=W)
    c = PAL["imagenes"] + (255,)
    d.ellipse((600, 330, 710, 440), fill=(0xFF, 0xC8, 0x2E, 255))
    d.polygon([(196, 784), (196, 700), (400, 480), (560, 650), (640, 570), (828, 740), (828, 784)], fill=c)
    # recorte redondeado inferior
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((196, 240, 828, 784), radius=70, fill=255)
    return mask


def terminal(d):
    g = (0x3E, 0xE8, 0x9B, 255)
    d.line([(300, 380), (470, 512), (300, 644)], fill=g, width=64, joint="curve")
    d.rounded_rectangle((520, 620, 740, 664), radius=22, fill=W)


DRAW = {"archivos": archivos, "notas": notas, "configuracion": configuracion,
        "calculadora": calculadora, "monitor": monitor, "imagenes": imagenes,
        "terminal": terminal}


def build(name):
    im, d = tile(PAL[name])
    if name == "imagenes":
        # dibujar el glifo sobre capa aparte para recortarlo a su marco
        layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        mask = imagenes(ld)
        layer.putalpha(Image.composite(layer.getchannel("A"), Image.new("L", (S, S), 0), mask))
        im.alpha_composite(layer)
    else:
        DRAW[name](d)
    return im.resize((512, 512), Image.LANCZOS)


def save_bmp32(im, path):
    a = np.asarray(im.convert("RGBA"))
    bgra = a[..., [2, 1, 0, 3]][::-1].copy()
    h, w = a.shape[:2]
    hdr = b"BM" + (54 + bgra.nbytes).to_bytes(4, "little") + b"\0\0\0\0" + (54).to_bytes(4, "little")
    dib = (40).to_bytes(4, "little") + w.to_bytes(4, "little") + h.to_bytes(4, "little") + \
        (1).to_bytes(2, "little") + (32).to_bytes(2, "little") + b"\0" * 24
    with open(path, "wb") as f:
        f.write(hdr + dib + bgra.tobytes())


def main():
    os.makedirs(OUT, exist_ok=True)
    icons = {n: build(n) for n in PAL}
    for n, im in icons.items():
        im.save(os.path.join(OUT, n + ".png"))
        if "--bmp" in sys.argv:
            save_bmp32(im.resize((128, 128), Image.LANCZOS), os.path.join(SYS, "app-%s.bmp" % n))
    # hoja de muestra sobre fondo oscuro y claro
    names = list(PAL)
    cell, pad = 200, 36
    sheet = Image.new("RGBA", (pad + len(names) * (cell + pad), 2 * (cell + 80) + pad), (0, 0, 0, 0))
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 22)
    except Exception:
        font = None
    for row, bg in enumerate(((0x0E, 0x14, 0x12), (0xF4, 0xF7, 0xF5))):
        y0 = pad + row * (cell + 80)
        ImageDraw.Draw(sheet).rectangle((0, y0 - pad // 2, sheet.width, y0 + cell + 80 - pad // 2), fill=bg + (255,))
        for i, n in enumerate(names):
            x = pad + i * (cell + pad)
            sheet.alpha_composite(icons[n].resize((cell, cell), Image.LANCZOS), (x, y0))
            col = (0xE8, 0xF2, 0xEE) if row == 0 else (0x14, 0x20, 0x1B)
            ImageDraw.Draw(sheet).text((x, y0 + cell + 14), n.capitalize(), fill=col + (255,), font=font)
    sheet.convert("RGB").save(os.path.join(OUT, "hoja_iconos.png"))
    print("OK", OUT)


main()
