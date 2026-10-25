#!/usr/bin/env python3
"""Genera un fondo azul con ondas suaves (original, procedural) en BMP 24 bpp.
Uso: gen_wallpaper.py [ancho alto] [salida.bmp]"""
import sys
import numpy as np
from PIL import Image

W = int(sys.argv[1]) if len(sys.argv) > 2 else 1920
H = int(sys.argv[2]) if len(sys.argv) > 2 else 1080
out = sys.argv[3] if len(sys.argv) > 3 else "wallpaper-aurora-blue.bmp"

y, x = np.mgrid[0:H, 0:W].astype(np.float64)
u, v = x / W, y / H

def sstep(a, b, t):
    t = np.clip((t - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)

# Altura de "cintas": senos con deformación de dominio
warp = 0.22 * np.sin(2.3 * v + 0.6) + 0.10 * np.sin(5.1 * u - 1.2 * v)
r1 = np.sqrt((u - 1.15) ** 2 + ((v + 0.35) * 0.9) ** 2)   # arcos desde arriba-derecha
r2 = np.sqrt((u + 0.25) ** 2 + ((v - 1.25) * 0.9) ** 2)   # y desde abajo-izquierda
h = (np.sin(9.0 * r1 + 1.3 * warp)
     + 0.9 * np.sin(7.0 * r2 - 0.8 * warp + 1.0)
     + 0.35 * np.sin(4.0 * (u + warp) + 1.7 * np.sin(1.7 * v + 0.4)))
h = (h - h.min()) / (h.max() - h.min())

# Sombreado por derivada (luz arriba-izquierda)
gy, gx = np.gradient(h)
light = np.clip(0.5 + 9.0 * (-gx - gy) * (W / 1920.0) * 22, 0, 1)

deep = np.array([6, 24, 120], float)
mid = np.array([22, 92, 235], float)
sky = np.array([150, 205, 255], float)
white = np.array([235, 245, 255], float)

t = sstep(0.0, 1.0, h)[..., None]
col = deep + (mid - deep) * t
col = col + (sky - col) * (sstep(0.55, 1.0, h) * 0.75)[..., None]
col = col + (white - col) * (np.clip(light - 0.62, 0, 1) * 1.6)[..., None]
col = col * (0.82 + 0.30 * light)[..., None]

# Viñeta + degradado general para dar profundidad
vig = 1.0 - 0.38 * (((u - 0.5) ** 2 + (v - 0.55) ** 2) * 2.2)
col = col * np.clip(vig, 0.55, 1.0)[..., None]
col += (np.random.RandomState(7).rand(H, W, 1) - 0.5) * 2.0  # dither anti-banding

img = Image.fromarray(np.clip(col, 0, 255).astype(np.uint8), "RGB")
img.save(out, format="BMP")
img.save(out.replace(".bmp", ".png"))
print(out, W, H)
