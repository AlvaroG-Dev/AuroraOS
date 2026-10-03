#!/bin/bash
#
# PNG → BMP 32-bit RGBA para Aurora OS
#
# Uso:
#   ./png_to_bmp.sh input.png output.bmp [size]
#   ./png_to_bmp.sh --batch src_dir dst_dir [size]
#
#   size puede ser:
#     auto       → mantiene el tamaño original
#     128        → 128x128 cuadrado (iconos)
#     1280x800   → dimensiones explícitas (wallpapers)
#     (defecto: 128)
#
# Ejemplos:
#   ./png_to_bmp.sh terminal.png system/icons/terminal-icon.bmp 128
#   ./png_to_bmp.sh wallpaper.png system/wallpapers/default-background.bmp auto
#   ./png_to_bmp.sh --batch raw/ system/icons/ 128
#

set -euo pipefail

# --- Detectar ImageMagick (v7 "magick" o v6 "convert") ---
if command -v magick >/dev/null 2>&1; then
    IM="magick"
elif command -v convert >/dev/null 2>&1; then
    IM="convert"
else
    echo "❌ Error: ImageMagick no está instalado." >&2
    echo "   Ubuntu/Debian:  sudo apt install imagemagick" >&2
    exit 1
fi

# --- Detectar python3 (para normalizar el header y verificar) ---
PYTHON=""
if command -v python3 >/dev/null 2>&1; then
    PYTHON="python3"
fi

# ---------------------------------------------------------------------------
# parse_size: "auto" | "N" | "WxH" -> "W H" o "" (auto)
# ---------------------------------------------------------------------------
parse_size() {
    local spec="$1"
    if [[ "$spec" == "auto" ]]; then
        echo ""
        return
    fi
    if [[ "$spec" =~ ^([0-9]+)x([0-9]+)$ ]]; then
        echo "${BASH_REMATCH[1]} ${BASH_REMATCH[2]}"
        return
    fi
    if [[ "$spec" =~ ^([0-9]+)$ ]]; then
        echo "${BASH_REMATCH[1]} ${BASH_REMATCH[1]}"
        return
    fi
    echo "ERROR"
}

# ---------------------------------------------------------------------------
# normalize_to_bmp_v3: reescribe un BMP v4/v5 (BI_BITFIELDS, header 124B)
# a BMP v3 (BITMAPINFOHEADER 40B, BI_RGB). Los píxeles BGRA no se tocan.
#
# El kernel de Aurora OS acepta ambos, pero v3 es más simple, pesa 84
# bytes menos y elimina cualquier posibilidad de que un lector se
# confunda con los masks/colorspace.
# ---------------------------------------------------------------------------
normalize_to_bmp_v3() {
    local file="$1"
    [[ -n "$PYTHON" ]] || return 0
    [[ -f "$file" ]] || return 0

    "$PYTHON" - "$file" <<'PYEOF'
import sys, struct

path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()

if len(data) < 54 or data[0:2] != b"BM":
    sys.exit(0)

data_offset = struct.unpack_from("<I", data, 10)[0]
info_size   = struct.unpack_from("<I", data, 14)[0]
width       = struct.unpack_from("<i", data, 18)[0]
height      = struct.unpack_from("<i", data, 22)[0]
planes      = struct.unpack_from("<H", data, 26)[0]
bpp         = struct.unpack_from("<H", data, 28)[0]
compression = struct.unpack_from("<I", data, 30)[0]

# Nada que hacer: ya es v3 BI_RGB.
if info_size == 40 and compression == 0:
    sys.exit(0)

# Solo tocamos BMPs de 32bpp con BI_BITFIELDS (v4/v5 de ImageMagick).
if bpp != 32 or compression != 3:
    sys.exit(0)

pixels = data[data_offset:]

new_data_offset = 14 + 40
new_file_size   = new_data_offset + len(pixels)

out = bytearray()
# BITMAPFILEHEADER
out += b"BM"
out += struct.pack("<I", new_file_size)
out += struct.pack("<HH", 0, 0)
out += struct.pack("<I", new_data_offset)
# BITMAPINFOHEADER
out += struct.pack("<I", 40)                     # header size
out += struct.pack("<i", width)
out += struct.pack("<i", height)                 # mantiene signo (top-down)
out += struct.pack("<H", planes)
out += struct.pack("<H", bpp)                    # 32
out += struct.pack("<I", 0)                      # BI_RGB
out += struct.pack("<I", len(pixels))
out += struct.pack("<i", 2835)
out += struct.pack("<i", 2835)
out += struct.pack("<I", 0)
out += struct.pack("<I", 0)
out += pixels

with open(path, "wb") as f:
    f.write(out)
PYEOF
}

# ---------------------------------------------------------------------------
# verify_bmp: imprime "W H BPP COMPRESSION" leyendo el header real.
# ---------------------------------------------------------------------------
verify_bmp() {
    local file="$1"
    if [[ -n "$PYTHON" ]]; then
        "$PYTHON" - "$file" <<'PYEOF'
import sys, struct
with open(sys.argv[1], "rb") as f:
    h = f.read(54)
w = struct.unpack_from("<i", h, 18)[0]
h_ = struct.unpack_from("<i", h, 22)[0]
bpp = struct.unpack_from("<H", h, 28)[0]
comp = struct.unpack_from("<I", h, 30)[0]
print(f"{w} {abs(h_)} {bpp} {comp}")
PYEOF
    else
        # Fallback: od sobre los mismos offsets.
        local w h bpp comp
        w=$(od -An -td4 -j18 -N4 "$file" | tr -d ' ')
        h=$(od -An -td4 -j22 -N4 "$file" | tr -d ' ')
        bpp=$(od -An -tu2 -j28 -N2 "$file" | tr -d ' ')
        comp=$(od -An -tu4 -j30 -N4 "$file" | tr -d ' ')
        [[ "$h" -lt 0 ]] && h=$(( -h ))
        echo "$w $h $bpp $comp"
    fi
}

# ---------------------------------------------------------------------------
# convert_one: el trabajo real.
# ---------------------------------------------------------------------------
convert_one() {
    local input="$1"
    local output="$2"
    local size_spec="$3"

    [[ -f "$input" ]] || { echo "❌ No existe $input" >&2; return 1; }

    mkdir -p "$(dirname "$output")"

    local tw th
    read -r tw th <<< "$(parse_size "$size_spec")"
    if [[ "$tw" == "ERROR" ]]; then
        echo "❌ Formato de tamaño inválido: '$size_spec'" >&2
        return 1
    fi

    local resize_opts=()
    if [[ -n "$tw" ]]; then
        resize_opts=(
            -resize "${tw}x${th}"
            -background none
            -gravity center
            -extent "${tw}x${th}"
        )
    fi

    echo "→ $input → $output  (size=$size_spec)"

    # ImageMagick 7 exige el prefijo "BMP4:" en el output; v6 lo
    # interpreta como nombre literal de archivo. Detectamos versión.
    local out_target="$output"
    [[ "$IM" == "magick" ]] && out_target="BMP4:$output"

    "$IM" "$input" \
        "${resize_opts[@]}" \
        -strip \
        -alpha on \
        -depth 8 \
        -define bmp:format=bmp4 \
        "$out_target"

    # Reescribir header v5 → v3 limpio.
    normalize_to_bmp_v3 "$output"

    # Verificación.
    local w h bpp comp size_bytes
    read -r w h bpp comp < <(verify_bmp "$output")
    size_bytes=$(stat -c%s "$output")

    if [[ "$bpp" != "32" ]]; then
        echo "⚠️  $bpp bpp (esperado 32). ¿El PNG tiene alpha?" >&2
    fi
    if [[ "$comp" != "0" ]]; then
        echo "⚠️  compression=$comp (esperado 0=BI_RGB)" >&2
    fi

    printf "✅ %s  (%sx%s, %s bpp, comp=%s, %s bytes)\n" \
        "$output" "$w" "$h" "$bpp" "$comp" "$size_bytes"

    if (( size_bytes > 250000 )); then
        echo "⚠️  >250 KB. Considera bajar SIZE." >&2
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Modo batch
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--batch" ]]; then
    SRC_DIR="${2:?Falta directorio origen}"
    DST_DIR="${3:?Falta directorio destino}"
    SIZE_SPEC="${4:-128}"

    [[ -d "$SRC_DIR" ]] || { echo "❌ No existe $SRC_DIR" >&2; exit 1; }
    mkdir -p "$DST_DIR"

    echo "=== Batch PNG → BMP ($SIZE_SPEC) ==="
    echo "Origen:  $SRC_DIR"
    echo "Destino: $DST_DIR"
    echo

    ok=0; fail=0
    for png in "$SRC_DIR"/*.png; do
        [[ -e "$png" ]] || continue
        base="$(basename "$png" .png)"
        if convert_one "$png" "$DST_DIR/$base.bmp" "$SIZE_SPEC"; then
            ok=$((ok+1))
        else
            fail=$((fail+1))
        fi
    done

    echo
    echo "=== Resultado: $ok OK, $fail fallos ==="
    exit 0
fi

# ---------------------------------------------------------------------------
# Modo single
# ---------------------------------------------------------------------------
INPUT="${1:?Uso: $0 input.png output.bmp [size]}"
OUTPUT="${2:?Uso: $0 input.png output.bmp [size]}"
SIZE_SPEC="${3:-128}"

convert_one "$INPUT" "$OUTPUT" "$SIZE_SPEC"