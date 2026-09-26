#!/bin/bash
#
# Recolorea un icono outline (negro sobre transparente) con el gradiente
# Aurora. Preserva el canal alpha del original.
#
# Uso:
#   ./recolor_aurora.sh input.png output.png [size]
#

set -euo pipefail

INPUT="${1:?Uso: $0 input.png output.png [size]}"
OUTPUT="${2:?Uso: $0 input.png output.png [size]}"
SIZE="${3:-120}"

[[ -f "$INPUT" ]] || { echo "❌ No existe $INPUT" >&2; exit 1; }

if command -v magick >/dev/null 2>&1; then IM="magick"; else IM="convert"; fi
mkdir -p "$(dirname "$OUTPUT")"

# --- 1. Redimensionar el icono original a SIZE x SIZE ---
#     -background none + -gravity center + -extent asegura un cuadrado
#     centrado aunque el PNG de origen no sea cuadrado.
"$IM" "$INPUT" \
    -resize "${SIZE}x${SIZE}" \
    -background none -gravity center \
    -extent "${SIZE}x${SIZE}" \
    /tmp/icon_resized.png

# --- 2. Extraer máscara de alpha (blanco = opaco, negro = transparente) ---
"$IM" /tmp/icon_resized.png -alpha extract -threshold 5% /tmp/aurora_mask.png

# --- 3. Construir el gradiente Aurora de 3 colores ---
#     IM6 no tiene gradiente de 3 colores; apilamos dos de 2 colores.
H2=$((SIZE * 2))
"$IM" -size "${SIZE}x${H2}" gradient:'#00D4FF-#7C4DFF' /tmp/g_top.png
"$IM" -size "${SIZE}x${H2}" gradient:'#7C4DFF-#FF4DD2' /tmp/g_bot.png
"$IM" /tmp/g_top.png /tmp/g_bot.png -append \
    -resize "${SIZE}x${SIZE}!" \
    /tmp/aurora_grad.png

# --- 4. Aplicar la máscara al gradiente ---
#     CopyOpacity copia el canal alpha del segundo al primero.
"$IM" /tmp/aurora_grad.png /tmp/aurora_mask.png \
    -alpha off -compose CopyOpacity -composite \
    "$OUTPUT"

# Limpieza
rm -f /tmp/icon_resized.png /tmp/aurora_mask.png \
      /tmp/g_top.png /tmp/g_bot.png /tmp/aurora_grad.png

printf "✅ %s\n" "$OUTPUT"