#!/bin/bash
# scripts/fetch-bin.sh — Copia binarios del host + sus librerías
# a precompiled/ y libs/glibc/, en el layout que el Makefile espera.
#
# Uso:
#   scripts/fetch-bin.sh /usr/bin/ls /usr/bin/cat /usr/bin/grep
#   scripts/fetch-bin.sh /usr/sbin/sshd   # también vale /sbin
#
# Efecto:
#   precompiled/usr/bin/<name>                 ← el binario
#   precompiled/usr/sbin/<name>                ← si venía de /sbin
#   libs/glibc/lib/x86_64-linux-gnu/*.so*      ← deps resueltas por ldd
#   libs/glibc/lib64/ld-linux-x86-64.so.2      ← el loader

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRECOMP="$ROOT/precompiled"
LIBS="$ROOT/libs/glibc"
LIBDIR="$LIBS/lib/x86_64-linux-gnu"

mkdir -p "$PRECOMP/usr/bin" "$PRECOMP/usr/sbin" \
         "$LIBDIR" "$LIBS/lib64"

# Loader dinámico (una vez).
if [ ! -e "$LIBS/lib64/ld-linux-x86-64.so.2" ]; then
    if [ -e /lib64/ld-linux-x86-64.so.2 ]; then
        cp -L /lib64/ld-linux-x86-64.so.2 "$LIBS/lib64/"
        echo "  loader: ld-linux-x86-64.so.2"
    fi
fi

# Copia recursiva de libs (algunas .so dependen de otras).
declare -A seen
copy_lib() {
    local lib="$1"
    local base
    base="$(basename "$lib")"
    [ -n "${seen[$base]:-}" ] && return
    seen[$base]=1

    case "$lib" in
        /lib/*|/usr/lib/*)
            if [ -e "$lib" ]; then
                cp -Lf "$lib" "$LIBDIR/$base"
                echo "  lib: $base"
                # Dependencias de la lib (transitivas).
                ldd "$lib" 2>/dev/null | awk '
                    /=>/  { print $3 }
                    /^\// { print $1 }
                ' | sort -u | while read -r dep; do
                    case "$dep" in
                        /lib/*|/usr/lib/*) copy_lib "$dep" ;;
                    esac
                done
            fi
            ;;
    esac
}

copy_bin() {
    local src="$1"
    local name
    name="$(basename "$src")"

    if [ ! -e "$src" ]; then
        echo "!! $src no existe, saltando"
        return 1
    fi

    # Destino: usr/bin o usr/sbin según origen.
    local dst_dir="$PRECOMP/usr/bin"
    case "$src" in
        /sbin/*|/usr/sbin/*) dst_dir="$PRECOMP/usr/sbin" ;;
    esac
    cp -fL "$src" "$dst_dir/$name"
    echo "+ bin: $dst_dir/$name"

    # Si es estático, no hace falta nada más.
    if ! file "$src" 2>/dev/null | grep -q "dynamically linked"; then
        echo "  (estático, sin deps)"
        return 0
    fi

    # Resolver deps.
    ldd "$src" 2>/dev/null | awk '
        /=>/  { print $3 }
        /^\// { print $1 }
    ' | sort -u | while read -r dep; do
        [ -z "$dep" ] && continue
        copy_lib "$dep"
    done
}

if [ $# -eq 0 ]; then
    echo "Uso: $0 <binario> [<binario> ...]"
    exit 1
fi

for arg in "$@"; do
    copy_bin "$arg" || true
done

echo
echo "Listo. Ahora:  make && make run-smp-kvm-ahci"