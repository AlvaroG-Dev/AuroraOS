#!/usr/bin/env python3
"""Fondo de AuroraOS: cielo nocturno con cortinas de aurora verde/turquesa
(procedural y original) en BMP 24 bpp. Uso: gen_wallpaper_aurora.py [ancho alto] [salida.bmp]"""
import sys
import numpy as np
from PIL import Image, ImageFilter

W = int(sys.argv[1]) if len(sys.argv) > 2 else 1920
H = int(sys.argv[2]) if len(sys.argv) > 2 else 1080
out = sys.argv[3] if len(sys.argv) > 3 else "wallpaper-aurora.bmp"

rs = np.random.RandomState(11)
y, x = np.mgrid[0:H, 0:W].astype(np.float64)
u, v = x / W, y / H

def sstep(a, b, t):
    t = np.clip((t - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)

# Cielo: casi negro con un matiz verdoso hacia el horizonte
top = np.array([3, 7, 10], float)
bot = np.array([6, 22, 24], float)
col = top + (bot - top) * (v ** 1.6)[..., None]

def curtain(base_y, amp, freq, phase, height, seed):
    r = np.random.RandomState(seed)
    # línea base ondulada (borde inferior brillante de la cortina)
    by = base_y + amp * np.sin(freq * u * 6.283 + phase) + 0.5 * amp * np.sin(2.3 * freq * u * 6.283 + 1.7 * phase)
    # rayos verticales: ruido 1D suavizado a lo largo de x
    n = r.rand(W // 6 + 2)
    n = np.convolve(n, np.ones(5) / 5, mode="same")
    rays = np.interp(np.arange(W), np.linspace(0, W, len(n)), n)
    rays = 0.55 + 0.9 * (rays - rays.min()) / (rays.max() - rays.min())
    d = (by - v)  # >0 por encima de la base
    up = np.exp(-np.clip(d, 0, None) / (height * rays[None, :]))
    below = np.exp(-np.clip(-d, 0, None) / 0.012)  # corte nítido por debajo
    inten = np.where(d >= 0, up, below) * sstep(-0.03, 0.02, d)
    # color: verde en la base, turquesa arriba
    k = np.clip(d / (height * 1.6), 0, 1)[..., None]
    green = np.array([62, 232, 155], float)
    teal = np.array([0, 212, 216], float)
    c = green + (teal - green) * k
    return c, inten

layers = [
    curtain(0.66, 0.10, 1.1, 0.4, 0.22, 3),
    curtain(0.78, 0.07, 1.6, 2.1, 0.14, 8),
    curtain(0.56, 0.05, 0.8, 4.0, 0.10, 5),
]
gains = [0.95, 0.60, 0.40]
add = np.zeros_like(col)
for (c, a), g in zip(layers, gains):
    add += c * (a * g)[..., None]
col = col + add * 0.8
col = 255 * (1 - np.exp(-col / 190))          # tone map suave

# Estrellas
stars = np.zeros((H, W))
for _ in range(int(W * H / 5500)):
    sx, sy = rs.randint(0, W), rs.randint(0, int(H * 0.7))
    stars[sy, sx] = rs.uniform(0.35, 1.0)
stars = np.asarray(Image.fromarray((stars * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7))).astype(float) / 255
col += (stars * 3.2 * (1 - sstep(0.1, 0.7, v)))[..., None] * 215

# Viñeta y dither anti-banding
vig = 1.0 - 0.42 * (((u - 0.5) ** 2 + (v - 0.5) ** 2) * 2.0)
col = col * np.clip(vig, 0.55, 1.0)[..., None]
col += (rs.rand(H, W, 1) - 0.5) * 2.0
img = Image.fromarray(np.clip(col, 0, 255).astype(np.uint8), "RGB")
img.save(out, format="BMP")
img.save(out.replace(".bmp", ".png"))
print(out, W, H)
