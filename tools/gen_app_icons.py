#!/usr/bin/env python3
"""Iconos de apps nativas de Aurora (BMP 32 bpp con alfa, 128x128).

Salida: assets/system/icons/taskmgr.bmp (monitor de rendimiento) y
app-generic.bmp (icono de reserva de Aurora para apps sin icono).
Baldosa verde-grafito con barras de actividad en verde aurora."""
import os
import numpy as np
from PIL import Image, ImageDraw


def save_bmp32(im, path):
    # BMP 32 bpp BGRA, filas de abajo arriba (lo que carga kernel/bmp.c)
    a = np.asarray(im.convert("RGBA"))
    bgra = a[..., [2, 1, 0, 3]][::-1].copy()
    h, w = a.shape[:2]
    hdr = b"BM" + (54 + bgra.nbytes).to_bytes(4, "little") + b"\0\0\0\0" + (54).to_bytes(4, "little")
    dib = (40).to_bytes(4, "little") + w.to_bytes(4, "little") + h.to_bytes(4, "little") + \
        (1).to_bytes(2, "little") + (32).to_bytes(2, "little") + b"\0" * 24
    with open(path, "wb") as f:
        f.write(hdr + dib + bgra.tobytes())


S = 512  # se dibuja grande y se reduce (antialias)
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((16, 16, S - 16, S - 16), radius=112, fill=(0x12, 0x1B, 0x18, 255))
d.rounded_rectangle((16, 16, S - 16, S - 16), radius=112, outline=(0x2A, 0x3D, 0x36, 255), width=6)
# cuadricula
for k in (1, 2, 3):
    y = 96 + k * 80
    d.line((72, y, S - 72, y), fill=(0x26, 0x35, 0x30, 255), width=4)
# barras
vals = [0.35, 0.55, 0.42, 0.7, 0.5, 0.85, 0.62]
bw, gap = 38, 14
x0 = (S - (len(vals) * bw + (len(vals) - 1) * gap)) // 2
base = S - 100
for i, v in enumerate(vals):
    h = int(300 * v)
    x = x0 + i * (bw + gap)
    d.rounded_rectangle((x, base - h, x + bw, base), radius=10, fill=(0x3E, 0xE8, 0x9B, 255))
im = im.resize((128, 128), Image.LANCZOS)
out = os.path.join(os.path.dirname(__file__), "..", "assets", "system", "icons", "taskmgr.bmp")
save_bmp32(im, out)
print("OK", out)


# ---- Icono generico: ventana con barra de titulo, en verde aurora ----
g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(g)
d.rounded_rectangle((16, 16, S - 16, S - 16), radius=112, fill=(0x12, 0x1B, 0x18, 255))
d.rounded_rectangle((16, 16, S - 16, S - 16), radius=112, outline=(0x2A, 0x3D, 0x36, 255), width=6)
d.rounded_rectangle((96, 120, S - 96, S - 120), radius=40, fill=(0x1C, 0x2A, 0x25, 255),
                    outline=(0x3E, 0xE8, 0x9B, 255), width=14)
d.rectangle((110, 182, S - 110, 190), fill=(0x3E, 0xE8, 0x9B, 255))
for i, c in enumerate(((0xFF, 0x6A, 0x3D), (0x4D, 0xA3, 0xFF), (0x3E, 0xE8, 0x9B))):
    x = 140 + i * 44
    d.ellipse((x - 14, 142, x + 14, 170), fill=c + (255,))
g = g.resize((128, 128), Image.LANCZOS)
save_bmp32(g, os.path.join(os.path.dirname(__file__), "..", "assets", "system", "icons", "app-generic.bmp"))
print("OK app-generic")
