#!/bin/bash
# scripts/nvidia-kpi-cc.sh — Plan_NVIDIA.md N2: compila los fuentes "nucleo" de
# kernel-open/nvidia contra LinuxKPI y resume los errores (que falta en la capa).
# Requiere scripts/nvidia-conftest.sh ejecutado. Uso: scripts/nvidia-kpi-cc.sh [fichero.c ...]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
NV=build/linuxkpi_src/nvidia-open; O=build/nvidia-kpi/obj
mkdir -p "$O"
# Copia de kernel-open/ con los parches minimos de kernel/linuxkpi/patches/nvidia/<fichero>.patch
# (el arbol descargado no se toca).
K=build/nvidia-kpi/ko
if [ ! -f $K/.aurora-ok ] || [ -n "$(find kernel/linuxkpi/patches/nvidia -newer $K/.aurora-ok -name '*.patch' 2>/dev/null)" ]; then
    rm -rf $K; cp -a $NV/kernel-open $K
    for p in kernel/linuxkpi/patches/nvidia/*.patch; do
        [ -f "$p" ] || continue
        b=$(basename "$p" .patch); t=$(find $K -name "$b" | head -1)
        patch -s "$t" "$p" || { echo "parche $p no aplica" >&2; exit 1; }
    done
    touch $K/.aurora-ok
fi
mkdir -p build/nvidia-kpi/out; echo "#define NV_COMPILER \"$(gcc -v 2>&1 | tail -n 1)\"" > build/nvidia-kpi/out/nv_compiler.h
cat > /tmp/nvkpi.mk <<'MK'
include kernel/linuxkpi/flags.mk
show:
	@echo $(LKPI_CFLAGS)
MK
FL="$(make -s -f /tmp/nvkpi.mk show) -DNVRM -DCONFIG_PROC_FS -DKBUILD_MODNAME=\"nvidia\" -DNV_VERSION_STRING=\"615.78.08\" -DNV_KERNEL_INTERFACE_LAYER -DNV_UVM_ENABLE -DNV_SPECTRE_V2=0 -DNVIDIA_UNDEF_LEGACY_BIT_MACROS -DLIBSPDM_CONFIG=\"nvspdm_rmconfig.h\" -DNDEBUG -ffreestanding -Wno-cast-qual -Wno-format-extra-args -Wno-error=undef -I$K/common/inc -I$K -I$K/nvidia -Ibuild/nvidia-kpi/out -I$NV/src/common/sdk/nvidia/inc -I$NV/src/common/inc -I$NV/src/nvidia/arch/nvalloc/unix/include -I$NV/src/nvidia/inc -I$NV/src/common/shared/inc"
CORE="nv nv-pci nv-dmabuf nv-nano-timer nv-acpi nv-dma nv-i2c nv-mmap nv-procfs nv-usermap nv-vm nv-vtophys os-interface os-mlock os-pci os-registry os-usermap nv-modeset-interface nv-pci-table nv-kthread-q nv-memdbg nv-report-err nv-rsync nv-msi nv-caps"
[ $# -gt 0 ] && CORE="$*"
ok=0; bad=0; : > $O/errors.txt
for f in $CORE; do
    f=${f%.c}
    if gcc $FL -c $K/nvidia/$f.c -o $O/$f.o 2> $O/$f.err; then ok=$((ok+1)); else bad=$((bad+1)); fi
    grep -E "error|fatal" $O/$f.err | sed "s|^[^ ]*/||;s|^|$f: |" >> $O/errors.txt
done
echo "compilan: $ok  con errores: $bad  ($(grep -c . $O/errors.txt) mensajes)"
echo "--- errores mas repetidos (sin el fichero):"
sed 's/^[^:]*: //;s/^[0-9]*:[0-9]*: //' $O/errors.txt | sort | uniq -c | sort -rn | head -${TOP:-25}
