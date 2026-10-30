#!/usr/bin/env python3
"""Limpia el emblema de AuroraOS (assets/brand/source/emblem_chatgpt.jpg):
extrae la silueta, la reescala con suavizado y la recolorea con un degradado
horizontal limpio (sin ruido JPEG ni halo). Genera PNG y BMP 32 bpp con alfa.
Uso: tools/gen_brand_assets.py"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets/brand/source/emblem_chatgpt.jpg"
OUT = ROOT / "assets/brand"
SCALE = 3

def sstep(a, b, t):
    t = np.clip((t - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)

img = Image.open(SRC).convert("RGB")
orig = np.asarray(img).astype(float)
lum = orig.max(axis=2)
mask0 = lum > 90
ys, xs = np.where(mask0)
x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()

# Degradado: color medio de las columnas opacas, suavizado.
cols = []
for x in range(x0, x1 + 1):
    sel = lum[:, x] > 190
    cols.append(orig[sel, x].mean(axis=0) if sel.any() else np.array([np.nan] * 3))
cols = np.array(cols)
for c in range(3):
    idx = np.arange(len(cols)); ok = ~np.isnan(cols[:, c])
    cols[:, c] = np.interp(idx, idx[ok], cols[ok, c])
k = np.ones(31) / 31
cols = np.stack([np.convolve(np.pad(cols[:, c], 15, mode="edge"), k, "valid") for c in range(3)], 1)
# Paleta de marca (theme.h): verde claro -> turquesa
g0, g1 = np.array([0x6B, 0xF0, 0x83], float), np.array([0x00, 0xD4, 0xD8], float)
print("extremo izq #%02X%02X%02X  extremo der #%02X%02X%02X" % (*g0.astype(int), *g1.astype(int)))

# Alfa de alta resolución a partir de la luminancia.
pad = 24
crop = img.crop((x0 - pad, y0 - pad, x1 + pad + 1, y1 + pad + 1))
big = crop.resize((crop.width * SCALE, crop.height * SCALE), Image.LANCZOS)
lb = np.asarray(big).astype(float).max(axis=2)
alpha = sstep(60, 150, lb)
alpha = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3.0))).astype(float) / 255
alpha = sstep(0.35, 0.65, alpha)  # recupera filo tras el desenfoque

H, W = alpha.shape
t = (np.arange(W) - pad * SCALE) / ((x1 - x0 + 1) * SCALE)
t = np.clip(t, 0, 1)[None, :, None]
rgb = g0 + (g1 - g0) * t
rgb = np.broadcast_to(rgb, (H, W, 3))
rgba = np.dstack([rgb, alpha[..., None] * 255]).clip(0, 255).astype(np.uint8)
emb = Image.fromarray(rgba, "RGBA")
emb.save(OUT / "aurora-emblem.png")

def fit(im, size):
    s = size / max(im.size)
    return im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.LANCZOS)

def square(im, size, margin=0.12):
    inner = fit(im, round(size * (1 - 2 * margin)))
    c = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    c.alpha_composite(inner, ((size - inner.width) // 2, (size - inner.height) // 2))
    return c

def save_bmp32(im, path):
    # BMP 32 bpp BGRA, filas de abajo arriba (lo que carga kernel/bmp.c)
    a = np.asarray(im.convert("RGBA"))
    bgra = a[..., [2, 1, 0, 3]][::-1].copy()
    h, w = a.shape[:2]
    hdr = b"BM" + (54 + bgra.nbytes).to_bytes(4, "little") + b"\0\0\0\0" + (54).to_bytes(4, "little")
    dib = (40).to_bytes(4, "little") + w.to_bytes(4, "little") + h.to_bytes(4, "little") + \
        (1).to_bytes(2, "little") + (32).to_bytes(2, "little") + b"\0" * 24
    path.write_bytes(hdr + dib + bgra.tobytes())

for s in (512, 256, 128, 64, 32):
    sq = square(emb, s)
    sq.save(OUT / f"aurora-emblem-{s}.png")
    if s in (256, 64, 32):
        save_bmp32(sq, OUT / f"aurora-emblem-{s}.bmp")

# Icono de app: baldosa redondeada #141F22 con borde tenue
S = 1024
tile = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(tile)
d.rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 0.225), fill=(0x14, 0x1F, 0x22, 255))
d.rounded_rectangle((2, 2, S - 3, S - 3), radius=int(S * 0.225), outline=(255, 255, 255, 28), width=3)
tile.alpha_composite(square(emb, S, margin=0.2))
tile.save(OUT / "aurora-icon-1024.png")
for s in (256, 128, 64):
    fit(tile, s).save(OUT / f"aurora-icon-{s}.png")
save_bmp32(fit(tile, 128), OUT / "aurora-icon-128.bmp")
save_bmp32(fit(tile, 64), OUT / "aurora-icon-64.bmp")
print("ok", emb.size)


# ---------------------------------------------------------------------------
# Lockup horizontal (emblema + wordmark "aurora OS")
# ---------------------------------------------------------------------------
LSRC = ROOT / "assets/brand/source/lockup_chatgpt.jpg"
if LSRC.exists():
    L = Image.open(LSRC).convert("RGB")
    EMB_X1 = 500  # el emblema acaba en x=474 y el texto empieza en x=527
    LS = 3
    lbig = L.resize((L.width * LS, L.height * LS), Image.LANCZOS)
    lum = np.asarray(lbig).astype(float).max(axis=2)
    a_raw = sstep(60, 150, lum)
    def soft(arr, sigma):
        b = np.asarray(Image.fromarray((arr * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(sigma))).astype(float) / 255
        return sstep(0.35, 0.65, b)
    cut = EMB_X1 * LS
    alpha = np.zeros_like(a_raw)
    alpha[:, :cut] = soft(a_raw, 3.0)[:, :cut]      # emblema: más suavizado
    alpha[:, cut:] = soft(a_raw, 1.4)[:, cut:]      # texto: respeta trazos finos
    Hh, Ww = alpha.shape
    ys, xs = np.where(alpha > 0.5)
    ex0 = xs[xs < cut].min(); ex1 = xs[xs < cut].max()
    t = np.clip((np.arange(Ww) - ex0) / (ex1 - ex0), 0, 1)[None, :, None]
    rgb = g0 + (g1 - g0) * t
    rgb = np.broadcast_to(rgb, (Hh, Ww, 3)).copy()
    rgb[:, cut:] = (0xF2, 0xF6, 0xF4)  # texto blanco roto
    out = Image.fromarray(np.dstack([rgb, alpha[..., None] * 255]).clip(0, 255).astype(np.uint8), "RGBA")
    out = out.crop(out.getbbox())
    pad = 40
    canvas = Image.new("RGBA", (out.width + 2 * pad, out.height + 2 * pad), (0, 0, 0, 0))
    canvas.alpha_composite(out, (pad, pad))
    canvas.save(OUT / "aurora-logo-horizontal.png")
    for h in (96, 48):
        s_ = h / canvas.height
        r = canvas.resize((round(canvas.width * s_), h), Image.LANCZOS)
        r.save(OUT / f"aurora-logo-horizontal-h{h}.png")
        save_bmp32(r, OUT / f"aurora-logo-horizontal-h{h}.bmp")
    print("lockup", canvas.size)
