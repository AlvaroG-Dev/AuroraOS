#!/bin/bash
# Capturas de pantalla de QEMU en cuanto un patron aparece en el log serie.
# Uso: QMON_SOCK=/ruta/socket scripts/qemu-shots.sh <patron> <retardo_s> <salida_base> [n=3] [intervalo=30]
# Genera <salida_base>-1.png ... (PNG via PIL). Pensado para lanzarse en segundo
# plano junto a tests/wayland/run-aurora-wayland.sh (AW_HOLD largo).
set -u
PAT=$1; DELAY=$2; BASE=$3; N=${4:-3}; EVERY=${5:-30}
LOG=${LOG:-/tmp/boot-test.log}
SOCK=${QMON_SOCK:-/tmp/qmon.sock}
until grep -aq -e "$PAT" "$LOG" 2>/dev/null; do sleep 3; done
sleep "$DELAY"
for i in $(seq 1 "$N"); do
  python3 -I - "$SOCK" "$BASE-$i.ppm" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.settimeout(3)
s.connect(sys.argv[1])
try: s.recv(4096)
except Exception: pass
s.send(("screendump " + sys.argv[2] + "\n").encode()); time.sleep(2)
PY
  python3 -I -c "
from PIL import Image
import sys
Image.open(sys.argv[1]).save(sys.argv[1].replace('.ppm','.png'))" "$BASE-$i.ppm" 2>/dev/null
  sleep "$EVERY"
done
