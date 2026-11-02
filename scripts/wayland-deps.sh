#!/bin/bash
# Lista las librerias que necesitan los binarios/librerias dados (resueltas con
# ldd contra las libs de la distro ya extraidas) y avisa de las que faltan en
# libs/glibc/. Con --copy las copia desde el host.
#
# Uso: scripts/wayland-deps.sh [--copy] fichero...
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="${WSTAGE:-$ROOT/build/wayland_stage/root}"
LIBDIR="$ROOT/libs/glibc/lib/x86_64-linux-gnu"
COPY=0
if [ "${1:-}" = "--copy" ]; then COPY=1; shift; fi
export LD_LIBRARY_PATH="$R/usr/lib/x86_64-linux-gnu"
for f in "$@"; do
  ldd "$f" 2>/dev/null | awk '/=>/ {print $3}'
done | sort -u | while read -r l; do
  [ -z "$l" ] && continue
  base="$(basename "$l")"
  if [ -e "$LIBDIR/$base" ]; then
    echo "have    $base"
  else
    echo "MISSING $base ($l)"
    if [ "$COPY" = 1 ] && [ -e "$l" ]; then
      case "$l" in
        "$R"*|/lib/*|/usr/lib/*) cp -Lf "$l" "$LIBDIR/$base" && echo "  copiada" ;;
      esac
    fi
  fi
done
