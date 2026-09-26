#!/bin/bash
#
# Aurora Icon Builder — conversión PURA
# PNG → BMP sin tocar un solo píxel.
# SVG → BMP con rasterizado a su tamaño natural.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SRC_DIR="$PROJ_ROOT/kernel/pre-icons"
DST_DIR="$PROJ_ROOT/assets/system/icons"

python3 -c "import PIL" 2>/dev/null || {
    echo "❌ Falta Pillow. sudo apt install python3-pil" >&2; exit 1
}

echo "=== Aurora Icon Builder (pure) ==="
echo "Origen:  $SRC_DIR"
echo "Destino: $DST_DIR"
echo

[[ -d "$SRC_DIR" ]] || { echo "❌ No existe $SRC_DIR" >&2; exit 1; }
mkdir -p "$DST_DIR"

shopt -s nullglob nocaseglob
sources=("$SRC_DIR"/*.svg "$SRC_DIR"/*.png)
shopt -u nullglob nocaseglob

if [[ ${#sources[@]} -eq 0 ]]; then
    echo "⚠️  No hay archivos en $SRC_DIR"; ls -la "$SRC_DIR"; exit 0
fi

echo "Encontrados ${#sources[@]} archivo(s)."
echo

ok=0; fail=0
for src in "${sources[@]}"; do
    base="$(basename "$src")"
    name="${base%.*}"
    out="$DST_DIR/$name.bmp"

    # Si es SVG, rasterizar UNA vez con rsvg-convert usando su tamaño natural.
    png_src="$src"
    tmp_png=""
    if [[ "${src,,}" == *.svg ]]; then
        tmp_png="$(mktemp --suffix=.png)"
        # SIN -w/-h: rsvg-convert usa el width/height del propio SVG.
        # Eso respeta el tamaño para el que fue diseñado.
        rsvg-convert "$src" -o "$tmp_png"
        png_src="$tmp_png"
    fi

    # Conversión PNG → BMP, directa, sin resize, sin máscara, sin nada.
    if python3 - "$png_src" "$out" <<'PYEOF'
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
img = Image.open(src).convert("RGBA")
w, h = img.size

# Bytes en orden RGBA tal cual los da PIL.
px = img.tobytes()

# BMP quiere BGRA. Reordenamos sin más.
bgra = bytearray(len(px))
bgra[0::4] = px[2::4]  # B ← R
bgra[1::4] = px[1::4]  # G ← G
bgra[2::4] = px[0::4]  # R ← B
bgra[3::4] = px[3::4]  # A ← A

import struct
row_size = w * 4
pix_size = row_size * h
file_size = 14 + 40 + pix_size

with open(dst, "wb") as f:
    f.write(b"BM")
    f.write(struct.pack("<I", file_size))
    f.write(struct.pack("<HH", 0, 0))
    f.write(struct.pack("<I", 14 + 40))
    f.write(struct.pack("<I", 40))
    f.write(struct.pack("<i", w))
    f.write(struct.pack("<i", -h))     # top-down (igual que los PNG)
    f.write(struct.pack("<H", 1))
    f.write(struct.pack("<H", 32))
    f.write(struct.pack("<I", 0))      # BI_RGB
    f.write(struct.pack("<I", pix_size))
    f.write(struct.pack("<i", 2835))
    f.write(struct.pack("<i", 2835))
    f.write(struct.pack("<I", 0))
    f.write(struct.pack("<I", 0))
    f.write(bytes(bgra))

print(f"  {w}x{h}  →  {dst}")
PYEOF
    then
        ok=$((ok+1))
    else
        echo "  ❌ fallo en $base" >&2
        fail=$((fail+1))
    fi

    [[ -n "$tmp_png" ]] && rm -f "$tmp_png"
done

echo
echo "=== $ok OK, $fail fallos ==="