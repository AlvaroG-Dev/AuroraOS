#!/bin/bash
# scripts/nvidia-link.sh — Plan_NVIDIA.md N2: enlaza build/nvidia-kpi/nvidia.kur
# = objetos de kernel-open/nvidia (scripts/nvidia-kpi-cc.sh) + nv-kernel.o (RM) +
# manifiesto .kur + stubs de lo que Aurora no tiene (libspdm/NVLink/NVSwitch/
# Tegra/UVM/IMEX). Cada stub anota su nombre en el log la primera vez y devuelve 0.
# Uso: scripts/nvidia-link.sh   (despues: lkpi.load=nvidia, en una maquina SIN GPU
# solo se comprueba que carga y se registra el driver PCI).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
B=build/nvidia-kpi; O=$B/obj
NK=build/linuxkpi_src/nvidia-open/src/nvidia/_out/Linux_x86_64/nv-kernel.o
[ -f "$NK" ] || { echo "falta $NK" >&2; exit 1; }
ls $O/*.o >/dev/null
CC=${KUR_CC:-gcc}
CF="-fcf-protection=none -ffreestanding -O2 -mcmodel=kernel -mno-red-zone -fno-stack-protector -fno-pic -fno-pie -fno-common -fno-asynchronous-unwind-tables -I. -Ikernel -Iinclude/linuxkpi"

$CC $CF -c kernel/linuxkpi/nvidia/nv_aurora_glue.c -o $B/glue.o
ld -r -o $B/nvidia-all.o $O/*.o $B/glue.o "$NK"
# Simbolos que ni LinuxKPI ni el kernel exportan.
nm build/linuxkpi/linuxkpi.o | awk '$2 ~ /[TDBRtdbrW]/{print $3}' > $B/have.txt
grep -o 'KEXPORT_SYMBOL([a-zA-Z_0-9]*' kernel/ksymtab_exports.c | sed 's/.*(//' >> $B/have.txt
nm $B/nvidia-all.o | awk '$2 ~ /[TDBRW]/{print $3}' >> $B/have.txt
sort -u -o $B/have.txt $B/have.txt
nm -u $B/nvidia-all.o | awk '{print $2}' | sort -u | grep -v '^__x86_indirect_thunk_' | comm -23 - $B/have.txt > $B/missing.txt
{
echo '// GENERADO por scripts/nvidia-link.sh — no editar'
echo 'extern void klog_printf(int level, const char *fmt, ...);'
echo 'static void hit(const char *n, unsigned char *seen) { if (!*seen) { *seen = 1; klog_printf(3, "[nvidia] stub llamado: %s", n); } }'
while read -r s; do echo "unsigned long $s(void) { static unsigned char seen; hit(\"$s\", &seen); return 0; }"; done < $B/missing.txt
for r in rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15; do
    echo "__asm__(\".text\\n.globl __x86_indirect_thunk_$r\\n__x86_indirect_thunk_$r:\\n\\tjmp *%$r\\n\");"
done
} > $B/nvidia_stubs.c
# Lo que debe dar LinuxKPI (lkpi_*) nunca se rellena con un stub: si falta, la
# capa esta desactualizada (make nvidia reconstruye linuxkpi.kur antes de enlazar).
if grep -q '^lkpi_' $B/missing.txt; then
    echo "faltan simbolos de LinuxKPI en build/linuxkpi/linuxkpi.o:" >&2
    grep '^lkpi_' $B/missing.txt >&2; exit 1
fi
$CC $CF -c $B/nvidia_stubs.c -o $B/stubs.o
$CC $CF -DKBUILD_MODNAME='"nvidia"' -c kernel/linuxkpi/kur_manifest.c -o $B/manifest.o
ld -r -o $B/nvidia.o $B/nvidia-all.o $B/stubs.o $B/manifest.o
[ -x tools/kurbuild ] || cc -O2 -Wall -iquote kernel -o tools/kurbuild tools/kurbuild.c kernel/sha256.c
./tools/kurbuild -H $B/nvidia.o $B/nvidia.kur
echo "OK: $B/nvidia.kur ($(du -h $B/nvidia.kur | cut -f1)); $(wc -l < $B/missing.txt) stubs"
echo "sin resolver tras el enlace:"; nm -u $B/nvidia.o | awk '{print $2}' | sort -u | comm -23 - $B/have.txt | grep -v __x86_indirect || true
