#!/bin/bash
# scripts/nvidia-drm-link.sh — enlaza build/nvidia-kpi/drm/nvidia-drm.kur
# = objetos de scripts/nvidia-drm-cc.sh + manifiesto + stubs de lo que ni
# LinuxKPI, ni lkpi_drm, ni nvidia-modeset dan. Carga tras lkpi_drm y nvidia-modeset.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
B=build/nvidia-kpi/drm; O=$B/obj
CC=${KUR_CC:-gcc}
CF="-fcf-protection=none -ffreestanding -O2 -mcmodel=kernel -mno-red-zone -fno-stack-protector -fno-pic -fno-pie -fno-common -fno-asynchronous-unwind-tables -I. -Ikernel -Iinclude/linuxkpi"
n=$(ls $O/*.o | wc -l)
[ "$n" -ge 20 ] || { echo "faltan objetos de nvidia-drm ($n de 20): mira $O/errors.txt" >&2; exit 1; }
ld -r -o $B/drm-all.o $O/*.o
{
nm build/linuxkpi/linuxkpi.o
nm build/linuxkpi/drv/lkpi_drm/*.o
nm build/nvidia-kpi/ms/ms-all.o
} | awk '$2 ~ /[TDBRtdbrW]/{print $3}' > $B/have.txt
grep -o 'KEXPORT_SYMBOL([a-zA-Z_0-9]*' kernel/ksymtab_exports.c | sed 's/.*(//' >> $B/have.txt
nm $B/drm-all.o | awk '$2 ~ /[TDBRW]/{print $3}' >> $B/have.txt
sort -u -o $B/have.txt $B/have.txt
nm -u $B/drm-all.o | awk '{print $2}' | sort -u | grep -v '^__x86_indirect_thunk_' | comm -23 - $B/have.txt > $B/missing.txt
{
echo '// GENERADO por scripts/nvidia-drm-link.sh — no editar'
echo 'extern void klog_printf(int level, const char *fmt, ...);'
echo 'static void hit(const char *n, unsigned char *seen) { if (!*seen) { *seen = 1; klog_printf(3, "[nvidia-drm] stub llamado: %s", n); } }'
while read -r s; do echo "unsigned long $s(void) { static unsigned char seen; hit(\"$s\", &seen); return 0; }"; done < $B/missing.txt
for r in rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15; do
    echo "__asm__(\".text\\n.globl __x86_indirect_thunk_$r\\n__x86_indirect_thunk_$r:\\n\\tjmp *%$r\\n\");"
done
} > $B/stubs.c
if grep -q '^lkpi_' $B/missing.txt; then
    echo "faltan simbolos de LinuxKPI:" >&2; grep '^lkpi_' $B/missing.txt >&2; exit 1
fi
$CC $CF -c $B/stubs.c -o $B/stubs.o
$CC $CF -DKBUILD_MODNAME='"nvidia_drm"' -c kernel/linuxkpi/kur_manifest.c -o $B/manifest.o
ld -r -o $B/nvidia-drm.o $B/drm-all.o $B/stubs.o $B/manifest.o
./tools/kurbuild -H $B/nvidia-drm.o $B/nvidia-drm.kur
echo "OK: $B/nvidia-drm.kur ($(du -h $B/nvidia-drm.kur | cut -f1)); $(wc -l < $B/missing.txt) stubs"
