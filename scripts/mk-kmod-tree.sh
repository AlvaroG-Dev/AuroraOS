#!/bin/bash
# scripts/mk-kmod-tree.sh — Genera /lib/modules/<release>/ para kmod
# (modprobe/rmmod/lsmod) a partir de los .kur compilados.
#
# kmod busca los módulos por nombre en modules.dep.bin, que genera `depmod`.
# depmod solo entiende ELF .ko de Linux, así que para cada .kur se crea un
# módulo "stub" mínimo que declara los mismos símbolos exportados/importados
# entre módulos (para que depmod calcule las dependencias) y se deja el
# .kur real como <release>/kernel/<nombre>.ko: el kernel solo mira el
# contenido, y finit_module() lo carga tal cual.
#
# Uso: scripts/mk-kmod-tree.sh <sysroot> <release> <dir-con-drivers/kur>
set -euo pipefail

SYSROOT="${1:?sysroot}"
REL="${2:?release}"
KUR_ROOT="${3:-drivers/kur}"

if ! command -v depmod >/dev/null 2>&1; then
    echo "[KMOD-TREE] depmod no encontrado: sin índice, modprobe no funcionará" >&2
    exit 0
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
STK="$STAGE/lib/modules/$REL/kernel"
DEST="$SYSROOT/lib/modules/$REL"
mkdir -p "$STK" "$DEST/kernel"

crc_of() { printf '0x%08x' "$(printf '%s' "$1" | cksum | cut -d' ' -f1)"; }

# Exports de cada módulo: __kur_exp_<sym> (ver KUR_EXPORT_SYMBOL).
declare -A is_export
mods=()
for d in "$KUR_ROOT"/*/; do
    name="$(basename "$d")"
    [ -f "$d/$name.kur" ] && [ -f "$d/$name.o" ] || continue
    mods+=("$name")
    for s in $(nm --defined-only "$d/$name.o" | awk '$3 ~ /^__kur_exp_/ { sub(/^__kur_exp_/, "", $3); print $3 }'); do
        is_export[$s]=1
    done
done

for name in "${mods[@]}"; do
    d="$KUR_ROOT/$name"
    stub="$STAGE/$name.c"
    : > "$stub"
    for s in $(nm --defined-only "$d/$name.o" | awk '$3 ~ /^__kur_exp_/ { sub(/^__kur_exp_/, "", $3); print $3 }'); do
        echo "__asm__(\".globl __crc_$s\\n.set __crc_$s, $(crc_of "$s")\\n\");" >> "$stub"
    done
    for s in $(nm -u "$d/$name.o" | awk '{print $2}'); do
        [ -n "${is_export[$s]:-}" ] || continue
        # struct modversion_info { unsigned long crc; char name[56]; } (x86-64: 64 bytes)
        echo "__asm__(\".section __versions,\\\"a\\\"\\n.align 8\\n.quad $(crc_of "$s")\\n.asciz \\\"$s\\\"\\n.fill $(( 56 - ${#s} - 1 )),1,0\\n.previous\\n\");" >> "$stub"
    done
    echo "int __kur_stub_$name;" >> "$stub"
    gcc -c -O0 "$stub" -o "$STK/$name.ko"
done

: > "$STAGE/lib/modules/$REL/modules.order"
: > "$STAGE/lib/modules/$REL/modules.builtin"
: > "$STAGE/lib/modules/$REL/modules.builtin.modinfo"
depmod -b "$STAGE" "$REL"

# Índices + .kur reales.
rm -f "$DEST"/modules.* "$DEST"/kernel/*.ko
cp -f "$STAGE/lib/modules/$REL"/modules.* "$DEST/"
for name in "${mods[@]}"; do
    cp -f "$KUR_ROOT/$name/$name.kur" "$DEST/kernel/$name.ko"
done
echo "[KMOD-TREE] $DEST: ${mods[*]}"
