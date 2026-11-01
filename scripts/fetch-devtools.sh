#!/bin/bash
# scripts/fetch-devtools.sh — Escalon A de Plan_GCC_Autocompilacion.md (G2):
# baja los .deb de Ubuntu 24.04 del toolchain nativo (gcc-13, g++-13, binutils,
# make, libc6-dev, linux-libc-dev...) SIN root (apt-get download + dpkg -x) y
# los deja en build/devtools/root. Con --img crea ademas build/aurora-devtools.img
# (FAT32; symlinks sustituidos por copias, porque FAT32 no los admite: G2.1).
#
# Uso:  scripts/fetch-devtools.sh [--img] [--size-mb N]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/devtools"
DEBS="$OUT/debs"
TREE="$OUT/root"
MAKE_IMG=0; SIZE_MB=1024
while [ $# -gt 0 ]; do
    case $1 in
        --img) MAKE_IMG=1 ;;
        --size-mb) SIZE_MB=$2; shift ;;
        *) echo "uso: $0 [--img] [--size-mb N]" >&2; exit 2 ;;
    esac
    shift
done

PKGS=(gcc-13 g++-13 cpp-13 binutils make libc6-dev linux-libc-dev
      libgcc-13-dev libstdc++-13-dev libgmp10 libmpfr6 libmpc3 libisl23
      zlib1g libzstd1 patch m4 nasm)

mkdir -p "$DEBS" "$TREE"
cd "$DEBS"

# Cierre transitivo de dependencias (solo Depends, sin recomendados). Se omiten
# los paquetes que ya trae la ISO (libc6, bash, coreutils...: libs/glibc/ y
# precompiled/) para no duplicarlos.
SKIP='^(libc6|libgcc-s1|libstdc\+\+6|bash|dash|coreutils|debconf|dpkg|perl-base|base-files|gcc-14-base|gcc-13-base|debianutils|sed|grep|tar|gzip|findutils|diffutils|libcrypt1)$'
mapfile -t ALL < <(apt-cache depends --recurse --no-recommends --no-suggests \
    --no-conflicts --no-breaks --no-replaces --no-enhances "${PKGS[@]}" \
    | grep '^[a-z0-9]' | grep -v ':i386' | sort -u | grep -Ev "$SKIP")
echo "[devtools] ${#ALL[@]} paquetes (con dependencias)"

for p in "${ALL[@]}"; do
    ls "${p}_"*.deb >/dev/null 2>&1 && continue
    apt-get download "$p" >/dev/null 2>&1 || echo "[devtools] AVISO: no se pudo bajar $p (virtual?)"
done

for d in *.deb; do dpkg -x "$d" "$TREE"; done
rm -rf "$TREE/usr/share/doc" "$TREE/usr/share/man" "$TREE/usr/share/info" \
       "$TREE/usr/share/locale" "$TREE/usr/share/lintian" "$TREE/usr/share/bug"
echo "[devtools] extraido en $TREE ($(du -sh "$TREE" | cut -f1))"
[ -x "$TREE/usr/bin/x86_64-linux-gnu-gcc-13" ] || echo "[devtools] AVISO: falta gcc-13 en el arbol"

if [ "$MAKE_IMG" = 1 ]; then
    IMG="$ROOT/build/aurora-devtools.img"
    FLAT="$OUT/flat"
    rm -rf "$FLAT"; mkdir -p "$FLAT"
    # FAT32 sin symlinks: cp -aL copia el destino de cada enlace.
    cp -aL "$TREE/usr/." "$FLAT/" 2>/dev/null || cp -rL "$TREE/usr/." "$FLAT/"
    rm -f "$IMG"; dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
    mkfs.fat -F 32 -n AUDEVTOOLS "$IMG" >/dev/null
    mcopy -i "$IMG" -s -Q "$FLAT"/* ::/ 2>&1 | tail -3 || true
    echo "[devtools] imagen: $IMG"
fi
