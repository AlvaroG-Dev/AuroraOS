#!/bin/bash
# scripts/nvidia-smoke.sh — Plan_NVIDIA.md T0.5: construye build/nvidia/nvsmoke.kur
# (nv-kernel.o de NVIDIA + capa de SO de mentira + tests/nvidia_smoke/nvsmoke.c).
# Requiere build/linuxkpi_src/nvidia-open (scripts/fetch-linux-drivers.sh) y
# haber compilado nv-kernel.o:  make -C build/linuxkpi_src/nvidia-open/src/nvidia -j4
# Sin hardware: carga el modulo en QEMU y mira `[nvsmoke] ALL PASS` en el log.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
NV=build/nvidia-smoke
O=build/linuxkpi_src/nvidia-open/src/nvidia/_out/Linux_x86_64/nv-kernel.o
[ -f "$O" ] || { echo "falta $O (compila src/nvidia primero)" >&2; exit 1; }
mkdir -p "$NV"
CC=${KUR_CC:-gcc}
CF="-fcf-protection=none -ffreestanding -O2 -mcmodel=kernel -mno-red-zone -fno-stack-protector -fno-pic -fno-pie -fno-common -fno-asynchronous-unwind-tables -I. -Ikernel"

# 1) Simbolos sin resolver de nv-kernel.o -> stubs. Cada stub registra (una vez)
#    su nombre en el log la primera vez que se llama y devuelve 0 (NV_OK/NULL/false).
{
echo '// GENERADO por scripts/nvidia-smoke.sh — no editar'
echo 'extern void klog_printf(int level, const char *fmt, ...);'
echo 'static void hit(const char *n, unsigned char *seen) { if (!*seen) { *seen = 1; klog_printf(3, "[nvsmoke] stub llamado: %s", n); } }'
nm -u "$O" | awk '{print $2}' | grep -v '^__x86_indirect_thunk_' | while read -r s; do
    echo "unsigned long $s(void) { static unsigned char seen; hit(\"$s\", &seen); return 0; }"
done
# nv_*/os_* con datos (variables) en vez de funciones:
nm -u "$O" | awk '{print $2}' | grep -E '^(nvidia_caps_root)$' >/dev/null && true
for r in rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15; do
    echo "__asm__(\".text\\n.globl __x86_indirect_thunk_$r\\n__x86_indirect_thunk_$r:\\n\\tjmp *%$r\\n\");"
done
} > "$NV/nvsmoke_stubs.c"
# Los simbolos de datos (extern NvU64 os_page_size...) no son funciones:
nm -u "$O" | awk '{print $2}' | grep -E '^os_(page_size|max_page_size|page_mask|page_shift|cc_|dma_buf_enabled|imex_channel_is_supported)' | while read -r s; do
    sed -i "/^unsigned long $s(void)/d" "$NV/nvsmoke_stubs.c"
    echo "unsigned long long $s;" >> "$NV/nvsmoke_stubs.c"
done
echo 'unsigned long long os_page_size_dummy;' >> "$NV/nvsmoke_stubs.c"

# 2) Compilar, enlazar y empaquetar.
$CC $CF -c "$NV/nvsmoke_stubs.c" -o "$NV/stubs.o"
$CC $CF -c tests/nvidia_smoke/nvsmoke.c -o "$NV/nvsmoke.o"
ld -r -o "$NV/nvsmoke_all.o" "$NV/nvsmoke.o" "$NV/stubs.o" "$O"
[ -x tools/kurbuild ] || cc -O2 -Wall -iquote kernel -o tools/kurbuild tools/kurbuild.c kernel/sha256.c
./tools/kurbuild -H "$NV/nvsmoke_all.o" "$NV/nvsmoke.kur"
echo "OK: $NV/nvsmoke.kur ($(du -h "$NV/nvsmoke.kur" | cut -f1))"
echo "sin resolver tras el enlace:"; nm -u "$NV/nvsmoke_all.o" | awk '{print $2}' | sort -u | head
