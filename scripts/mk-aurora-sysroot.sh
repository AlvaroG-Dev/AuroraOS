#!/bin/bash
# scripts/mk-aurora-sysroot.sh — Sysroot de desarrollo para el cross x86_64-pc-aurora-gnu.
# Combina los archivos de desarrollo de glibc 2.39 (de fetch-devtools.sh) con las
# bibliotecas de ejecucion que ya van en la ISO (libs/glibc/). Distribucion tipo
# Ubuntu/multiarch (/usr/include/x86_64-linux-gnu, /usr/lib/x86_64-linux-gnu).
# Uso: scripts/mk-aurora-sysroot.sh [destino]    (por defecto build/aurora-sysroot)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/devtools/root"
DST="${1:-$ROOT/build/aurora-sysroot}"
[ -f "$SRC/usr/lib/x86_64-linux-gnu/libc.so" ] || { echo "falta $SRC: ejecuta scripts/fetch-devtools.sh" >&2; exit 1; }
rm -rf "$DST"; mkdir -p "$DST/usr/lib" "$DST/usr/include"
cp -a "$SRC/usr/include/." "$DST/usr/include/"
cp -a "$SRC/usr/lib/x86_64-linux-gnu" "$DST/usr/lib/"
# Runtime (misma glibc que la ISO)
for f in libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1 libutil.so.1 libresolv.so.2 libgcc_s.so.1; do
    if [ -e "$ROOT/libs/glibc/lib/x86_64-linux-gnu/$f" ]; then
        cp -aL "$ROOT/libs/glibc/lib/x86_64-linux-gnu/$f" "$DST/usr/lib/x86_64-linux-gnu/$f"
    elif [ -e "/lib/x86_64-linux-gnu/$f" ]; then   # p. ej. libmvec.so.1: no va en la ISO (AS_NEEDED en libm.so)
        cp -aL "/lib/x86_64-linux-gnu/$f" "$DST/usr/lib/x86_64-linux-gnu/$f"; echo "[sysroot] aviso: $f tomada del host (no esta en libs/glibc)"
    fi
done
mkdir -p "$DST/usr/lib64"; cp -aL "$ROOT/libs/glibc/lib64/ld-linux-x86-64.so.2" "$DST/usr/lib64/"
# merged-usr: /lib -> usr/lib, /lib64 -> usr/lib64 (los scripts de ld usan /lib/x86_64-linux-gnu)
ln -sfn usr/lib "$DST/lib"; ln -sfn usr/lib64 "$DST/lib64"
echo "[sysroot] $DST ($(du -sh "$DST" | cut -f1))"
