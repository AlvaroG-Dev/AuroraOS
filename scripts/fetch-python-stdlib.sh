#!/bin/bash
# scripts/fetch-python-stdlib.sh — Copia el stdlib de Python del host
# al sysroot en el layout que el intérprete espera.
#
# Uso:
#   scripts/fetch-python-stdlib.sh
#
# Detecta la versión del python3 del host (debe coincidir con el
# binario que ya vive en sysroot/usr/bin/python3). Copia todo el
# stdlib EXCEPTO: tests, GUI, herramientas de desarrollo, cache.
#
# Efecto:
#   sysroot/usr/lib/python3.X/                ← el stdlib
#   sysroot/usr/lib/python3.X/lib-dynload/    ← módulos C (.so)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="$ROOT/sysroot"

# Detectar versión del host.
HOST_PY="$(command -v python3 || true)"
if [ -z "$HOST_PY" ]; then
    echo "!! no encuentro python3 en el host" >&2
    exit 1
fi

HOST_VER="$("$HOST_PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
SRC="$("$HOST_PY" -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')"

if [ ! -d "$SRC" ]; then
    echo "!! stdlib del host no encontrado en $SRC" >&2
    exit 1
fi

DST="$SYSROOT/usr/lib/python${HOST_VER}"

echo "Host python:  $HOST_PY"
echo "Versión:      $HOST_VER"
echo "Origen:       $SRC"
echo "Destino:      $DST"

# Comprobar que coincide con el binario del sysroot (si está).
if [ -x "$SYSROOT/usr/bin/python3" ]; then
    SYS_VER="$("$SYSROOT/usr/bin/python3" -c \
        'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null || true)"
    if [ -n "$SYS_VER" ] && [ "$SYS_VER" != "$HOST_VER" ]; then
        echo "!! aviso: el python3 de sysroot es $SYS_VER, el host es $HOST_VER" >&2
        echo "   Los .pyc no coincidirán y Python podría quejarse." >&2
    fi
fi

mkdir -p "$DST"

# rsync con exclusiones: tests, GUI, herramientas de dev, cache.
# --delete deja el sysroot idéntico al host para esta versión.
rsync -a --delete \
  --exclude 'test/' \
  --exclude 'tests/' \
  --exclude 'tkinter/' \
  --exclude 'idlelib/' \
  --exclude 'lib2to3/' \
  --exclude 'ensurepip/' \
  --exclude 'distutils/' \
  --exclude 'venv/' \
  --exclude '__pycache__/' \
  --exclude '*.pyc' \
  --exclude '*.pyo' \
  --exclude 'site-packages/' \
  "$SRC/" "$DST/"

# Precompilar .py → .pyc. Reduce el arranque de Python ~5x y evita que
# Python intente escribir __pycache__ en el rootfs RO.
# Sin -f: compileall respeta mtimes y salta los .pyc ya actualizados,
# así que repetir make image no recompila nada.
echo
echo "Precompilando stdlib a .pyc (unchecked-hash)..."
"$HOST_PY" -m compileall \
    --invalidation-mode unchecked-hash \
    -q -j 0 \
    "$DST" || {
    echo "!! compileall falló" >&2
    exit 1
}
PYC_COUNT="$(find "$DST" -name '*.pyc' | wc -l)"
echo "  .pyc generados: $PYC_COUNT"

# Resumen de tamaño.
echo
echo "Tamaño copiado:"
du -sh "$DST"
echo
echo "Módulos críticos:"
for f in encodings/__init__.py os.py sysconfig.py site.py abc.py; do
    if [ -e "$DST/$f" ]; then
        echo "  OK  $f"
    else
        echo "  FALTA $f"
    fi
done

echo
echo "Listo. Ahora:  make image && make run-smp-kvm-ahci"