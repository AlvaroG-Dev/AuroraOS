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

# Version de Python del GUEST: la del binario precompilado (python3.X). El
# stdlib debe ser de esa misma version; si el host trae otra (p. ej. 3.14 en
# Kali), se busca un python3.X en el host y, si no hay, se omite sin romper
# la build (la ISO arranca igual, solo sin stdlib de Python).
GUEST_VER=""
for f in "$ROOT"/precompiled/usr/bin/python3.[0-9]*; do
    [ -e "$f" ] || continue
    GUEST_VER="${f##*/python}"
    break
done

HOST_PY=""
if [ -n "$GUEST_VER" ]; then
    HOST_PY="$(command -v "python$GUEST_VER" || true)"
fi
if [ -z "$HOST_PY" ]; then
    CAND="$(command -v python3 || true)"
    if [ -n "$CAND" ]; then
        CAND_VER="$("$CAND" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
        if [ -z "$GUEST_VER" ] || [ "$CAND_VER" = "$GUEST_VER" ]; then
            HOST_PY="$CAND"
        fi
    fi
fi
if [ -z "$HOST_PY" ]; then
    echo "!! aviso: el guest usa Python ${GUEST_VER:-?} y el host no tiene esa version;" >&2
    echo "   se omite el stdlib de Python (instala python${GUEST_VER:-3} en el host para incluirlo)." >&2
    exit 0
fi

HOST_VER="$("$HOST_PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
SRC="$("$HOST_PY" -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')"

if [ ! -d "$SRC" ]; then
    echo "!! stdlib del host no encontrado en $SRC" >&2
    exit 0
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
if command -v rsync >/dev/null 2>&1; then
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
else
  # Sin rsync (p. ej. compilando dentro de AuroraOS): tar con las mismas
  # exclusiones; se vacia DST para igualar el efecto de --delete.
  rm -rf "$DST"; mkdir -p "$DST"
  tar -C "$SRC" --exclude 'test/' --exclude 'tests/' --exclude 'tkinter/' --exclude 'idlelib/' --exclude 'lib2to3/' --exclude 'ensurepip/' --exclude 'distutils/' --exclude 'venv/' --exclude '__pycache__/' --exclude '*.pyc' --exclude '*.pyo' --exclude 'site-packages/' -cf - . | tar -C "$DST" -xf -
fi

# Precompilar .py → .pyc. Reduce el arranque de Python ~5x y evita que
# Python intente escribir __pycache__ en el rootfs RO.
# Sin -f: compileall respeta mtimes y salta los .pyc ya actualizados,
# así que repetir make image no recompila nada.
echo
echo "Precompilando stdlib a .pyc (unchecked-hash)..."
# -j 0 usa multiprocessing (semaforos POSIX); si no funciona (p. ej. dentro de
# AuroraOS) se reintenta en un solo proceso.
"$HOST_PY" -m compileall \
    --invalidation-mode unchecked-hash \
    -q -j 0 \
    "$DST" 2>/tmp/compileall.err || {
    echo "   compileall -j 0 fallo; reintentando con -j 1"
    "$HOST_PY" -m compileall \
        --invalidation-mode unchecked-hash \
        -q -j 1 \
        "$DST" || {
        echo "!! compileall falló" >&2
        exit 1
    }
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