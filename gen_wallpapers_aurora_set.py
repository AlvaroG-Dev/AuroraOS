#!/usr/bin/env python3
"""Fondos suaves de AuroraOS (procedurales, 1280x720 BMP 24 bpp; el kernel los
escala). Uso: gen_wallpapers_aurora_set.py [directorio_salida]"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageFilter

W, H = 1280, 720
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "assets/system/wallpapers")
OUT.mkdir(parents=True, exist_ok=True)
y, x = np.mgrid[0:H, 0:W].astype(np.float64)
u, v = x / W, y / H
GREEN = np.array([62, 232, 155.0]); TEAL = np.array([0, 212, 216.0]); LIME = np.array([107, 240, 131.0])

def sstep(a, b, t):
    t = np.clip((t - a) / (b - a), 0, 1); return t * t * (3 - 2 * t)

def blur(a, r):
    im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
    return np.asarray(im.filter(ImageFilter.GaussianBlur(r))).astype(float)

def stars(seed, n, ymax=0.7, gain=200):
    rs = np.random.RandomState(seed)
    s = np.zeros((H, W))
    for _ in range(n):
        s[rs.randint(0, int(H * ymax)), rs.randint(0, W)] = rs.uniform(0.3, 1.0)
    s = np.asarray(Image.fromarray((s * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))).astype(float) / 255
    return (s * 3.0 * gain)[..., None]

def finish(col, name, seed=3, vig_amt=0.35):
    rs = np.random.RandomState(seed)
    vig = 1.0 - vig_amt * (((u - 0.5) ** 2 + (v - 0.5) ** 2) * 2.0)
    col = col * np.clip(vig, 0.6, 1.0)[..., None]
    col += (rs.rand(H, W, 1) - 0.5) * 2.0
    img = Image.fromarray(np.clip(col, 0, 255).astype(np.uint8), "RGB")
    img.save(OUT / f"{name}.bmp", format="BMP")
    return img

def glow(cx, cy, rx, ry, color, power=1.0):
    d = np.sqrt(((u - cx) / rx) ** 2 + ((v - cy) / ry) ** 2)
    return (np.exp(-(d ** 2) * 2.2) ** power)[..., None] * color

# 1) Calma: bandas horizontales difusas, sin rayos
top = np.array([4, 9, 12.0]); bot = np.array([7, 24, 26.0])
col = top + (bot - top) * (v ** 1.4)[..., None]
band = lambda cy, th, amp, ph: np.exp(-(((v - cy - amp * np.sin(2 * np.pi * (u * 0.9 + ph))) / th) ** 2))
b1 = band(0.58, 0.07, 0.05, 0.1); b2 = band(0.48, 0.05, 0.04, 0.55); b3 = band(0.68, 0.06, 0.03, 0.8)
col += (b1[..., None] * GREEN * 0.55 + b2[..., None] * TEAL * 0.38 + b3[..., None] * LIME * 0.22)
col = 255 * (1 - np.exp(-col / 170)); col += stars(1, 380, gain=130) * (1 - sstep(0.1, 0.6, v))[..., None]
finish(col, "aurora-calma")

# 2) Noche: casi negro, resplandor verde bajo, siluetas lejanas
top = np.array([2, 5, 8.0]); bot = np.array([4, 12, 14.0])
col = top + (bot - top) * (v ** 1.2)[..., None]
col += glow(0.5, 0.86, 0.75, 0.30, GREEN * 0.55) + glow(0.30, 0.9, 0.5, 0.2, TEAL * 0.35)
ridge = 0.80 + 0.04 * np.sin(2 * np.pi * (u * 1.3 + 0.2)) + 0.02 * np.sin(2 * np.pi * (u * 3.1 + 0.6))
mask = sstep(-0.004, 0.004, v - ridge)[..., None]
col = col * (1 - mask) + np.array([2, 6, 7.0]) * mask
col = 255 * (1 - np.exp(-col / 190)); col += stars(2, 520, gain=190) * (1 - sstep(0.15, 0.75, v))[..., None]
finish(col, "aurora-noche")

# 3) Bruma: manchas suaves tipo degradado de malla
base = np.array([5, 14, 17.0])
col = np.broadcast_to(base, (H, W, 3)).copy()
for cx, cy, rx, ry, c, g in [(0.2, 0.25, 0.55, 0.55, GREEN, 0.50), (0.85, 0.2, 0.5, 0.5, TEAL, 0.42),
                             (0.6, 0.85, 0.7, 0.5, LIME, 0.28), (0.05, 0.95, 0.4, 0.4, TEAL, 0.25)]:
    col += glow(cx, cy, rx, ry, c * g)
col = 255 * (1 - np.exp(-col / 200))
finish(col, "aurora-bruma")

# 4) Cima: cresta de montañas con aurora tenue de baja frecuencia
top = np.array([3, 8, 12.0]); bot = np.array([8, 26, 28.0])
col = top + (bot - top) * (v ** 1.5)[..., None]
r = np.random.RandomState(5)
n = np.interp(np.arange(W), np.linspace(0, W, 14), r.rand(14))
n = blur(np.tile(n, (8, 1)) * 255, 12)[0] / 255
cur = np.exp(-np.clip(0.66 - v, 0, None) / (0.20 * (0.6 + 0.7 * n[None, :]))) * sstep(0.0, 0.04, 0.66 - v)
col += cur[..., None] * (GREEN * 0.5 + (TEAL - GREEN) * np.clip((0.66 - v) * 2.2, 0, 1)[..., None] * 0.5)
for k, (base_y, amp, f, ph, shade) in enumerate([(0.74, 0.05, 1.2, 0.3, [6, 20, 21.0]), (0.82, 0.045, 1.9, 0.9, [4, 13, 14.0]), (0.91, 0.03, 2.6, 0.1, [2, 7, 8.0])]):
    rg = base_y + amp * np.sin(2 * np.pi * (u * f + ph)) + amp * 0.4 * np.sin(2 * np.pi * (u * f * 2.7 + ph * 2))
    m = sstep(-0.003, 0.003, v - rg)[..., None]
    col = col * (1 - m) + np.array(shade) * m
col = 255 * (1 - np.exp(-col / 190)); col += stars(4, 330, ymax=0.5, gain=160) * (1 - sstep(0.1, 0.55, v))[..., None]
finish(col, "aurora-cima")

# 5) Grafito: gris oscuro neutro con una sola cinta verde fina (minimalista)
top = np.array([14, 17, 18.0]); bot = np.array([8, 10, 11.0])
col = top + (bot - top) * v[..., None]
cy = 0.62 + 0.10 * np.sin(2 * np.pi * (u * 0.8 + 0.15)) - 0.05 * u
rib = np.exp(-(((v - cy) / 0.006) ** 2))
halo = np.exp(-(((v - cy) / 0.07) ** 2))
t = np.clip(u, 0, 1)[..., None]
cc = LIME + (TEAL - LIME) * t
col += rib[..., None] * cc * 1.2 + halo[..., None] * cc * 0.10
col = 255 * (1 - np.exp(-col / 200))
finish(col, "aurora-grafito")


# 6) Alba (modo claro): menta muy pálida con velos de aurora y colinas suaves
top = np.array([238, 247, 244.0]); bot = np.array([205, 232, 226.0])
col = top + (bot - top) * (v ** 1.3)[..., None]
for cx, cy, rx, ry, c, g in [(0.25, 0.30, 0.55, 0.45, np.array([150, 232, 185.0]), 0.55),
                             (0.80, 0.22, 0.50, 0.45, np.array([140, 222, 232.0]), 0.50),
                             (0.55, 0.62, 0.70, 0.35, np.array([170, 238, 200.0]), 0.35)]:
    d = np.sqrt(((u - cx) / rx) ** 2 + ((v - cy) / ry) ** 2)
    a = (np.exp(-(d ** 2) * 2.0) * g)[..., None]
    col = col * (1 - a) + c * a
for base_y, amp, f, ph, shade, g in [(0.80, 0.045, 1.1, 0.2, np.array([176, 218, 208.0]), 0.55),
                                     (0.89, 0.035, 1.8, 0.7, np.array([150, 200, 190.0]), 0.65)]:
    rg = base_y + amp * np.sin(2 * np.pi * (u * f + ph)) + amp * 0.4 * np.sin(2 * np.pi * (u * f * 2.6 + ph * 2))
    m = (sstep(-0.004, 0.004, v - rg) * g)[..., None]
    col = col * (1 - m) + shade * m
finish(col, "aurora-alba", vig_amt=0.04)

# hoja de contacto
names = ["aurora-calma", "aurora-noche", "aurora-bruma", "aurora-cima", "aurora-grafito", "aurora-alba"]
tw, th = 640, 360
sheet = Image.new("RGB", (tw * 2 + 30, th * 3 + 40), (12, 14, 14))
for i, n_ in enumerate(names):
    im = Image.open(OUT / f"{n_}.bmp").resize((tw, th), Image.LANCZOS)
    sheet.paste(im, (10 + (i % 2) * (tw + 10), 10 + (i // 2) * (th + 10)))
sheet.save(OUT / "_contact.png")
print("ok", names)
