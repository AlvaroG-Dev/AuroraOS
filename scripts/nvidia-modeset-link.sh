#!/bin/bash
# scripts/nvidia-modeset-link.sh — enlaza build/nvidia-kpi/ms/nvidia-modeset.kur
# = capa de SO (scripts/nvidia-modeset-cc.sh) + nv-modeset-kernel.o (nvkms, de
# src/nvidia-modeset) + manifiesto .kur + stubs de lo que Aurora no tiene. Los
# simbolos que exporta nvidia.kur (nvidia_get_rm_ops...) los resuelve el cargador.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
B=build/nvidia-kpi/ms; O=$B/obj
# Por defecto nvkms con DEBUG=1 (trazas [kapi] de nvKmsKapiLogDebug y
# nvEvoLog*Debug) mientras dura el bring-up; NVKMS_DEBUG=0 usa el build release.
if [ "${NVKMS_DEBUG:-1}" = 1 ]; then OUTD=dbg; else OUTD=Linux_x86_64; fi
NM=build/linuxkpi_src/nvidia-open/src/nvidia-modeset/_out/$OUTD/nv-modeset-kernel.o
[ -f "$NM" ] || { echo "falta $NM" >&2; exit 1; }
CC=${KUR_CC:-gcc}
CF="-fcf-protection=none -ffreestanding -O2 -mcmodel=kernel -mno-red-zone -fno-stack-protector -fno-pic -fno-pie -fno-common -fno-asynchronous-unwind-tables -I. -Ikernel -Iinclude/linuxkpi"
strip --strip-debug -o $B/nv-modeset-kernel.o "$NM"
ld -r -o $B/ms-all.o $O/nvidia-modeset-linux.o $O/nv-kthread-q.o $B/nv-modeset-kernel.o
nm build/linuxkpi/linuxkpi.o | awk '$2 ~ /[TDBRtdbrW]/{print $3}' > $B/have.txt
grep -o 'KEXPORT_SYMBOL([a-zA-Z_0-9]*' kernel/ksymtab_exports.c | sed 's/.*(//' >> $B/have.txt
# exports de nvidia.kur (EXPORT_SYMBOL en kernel-open/nvidia)
printf '%s\n' nvidia_get_rm_ops nvidia_register_error_cb nvidia_unregister_error_cb >> $B/have.txt
nm $B/ms-all.o | awk '$2 ~ /[TDBRW]/{print $3}' >> $B/have.txt
sort -u -o $B/have.txt $B/have.txt
nm -u $B/ms-all.o | awk '{print $2}' | sort -u | grep -v '^__x86_indirect_thunk_' | comm -23 - $B/have.txt > $B/missing.txt
{
echo '// GENERADO por scripts/nvidia-modeset-link.sh — no editar'
echo 'extern void klog_printf(int level, const char *fmt, ...);'
echo 'static void hit(const char *n, unsigned char *seen) { if (!*seen) { *seen = 1; klog_printf(3, "[nvidia-modeset] stub llamado: %s", n); } }'
while read -r s; do echo "unsigned long $s(void) { static unsigned char seen; hit(\"$s\", &seen); return 0; }"; done < $B/missing.txt
for r in rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15; do
    echo "__asm__(\".text\\n.globl __x86_indirect_thunk_$r\\n__x86_indirect_thunk_$r:\\n\\tjmp *%$r\\n\");"
done
} > $B/stubs.c
if grep -q '^lkpi_' $B/missing.txt; then
    echo "faltan simbolos de LinuxKPI:" >&2; grep '^lkpi_' $B/missing.txt >&2; exit 1
fi
$CC $CF -c $B/stubs.c -o $B/stubs.o
$CC $CF -DKBUILD_MODNAME='"nvidia_modeset"' -c kernel/linuxkpi/kur_manifest.c -o $B/manifest.o
ld -r -o $B/nvidia-modeset.o $B/ms-all.o $B/stubs.o $B/manifest.o
[ -x tools/kurbuild ] || cc -O2 -Wall -iquote kernel -o tools/kurbuild tools/kurbuild.c kernel/sha256.c
./tools/kurbuild -H $B/nvidia-modeset.o $B/nvidia-modeset.kur
echo "OK: $B/nvidia-modeset.kur ($(du -h $B/nvidia-modeset.kur | cut -f1)); $(wc -l < $B/missing.txt) stubs"
