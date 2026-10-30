#!/bin/bash
# scripts/nvidia-modeset-cc.sh — compila la capa de SO de nvidia-modeset
# (kernel-open/nvidia-modeset) contra LinuxKPI. Requiere scripts/nvidia-kpi-cc.sh
# ejecutado antes (copia parcheada de kernel-open en build/nvidia-kpi/ko).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
NV=build/linuxkpi_src/nvidia-open; K=build/nvidia-kpi/ko; O=build/nvidia-kpi/ms/obj
mkdir -p "$O"
cat > build/nvidia-kpi/nvkpi-flags.mk <<'MK'
include kernel/linuxkpi/flags.mk
show:
	@echo $(LKPI_CFLAGS)
MK
FL="$(make -s -f build/nvidia-kpi/nvkpi-flags.mk show) -DNVRM -DCONFIG_PROC_FS -DKBUILD_MODNAME=\"nvidia-modeset\" -DNV_VERSION_STRING=\"615.78.08\" -DNV_KERNEL_INTERFACE_LAYER -DNV_SPECTRE_V2=0 -DNVIDIA_UNDEF_LEGACY_BIT_MACROS -UDEBUG -U_DEBUG -DNDEBUG -DNV_BUILD_MODULE_INSTANCES=0 -include linux/mm.h -ffreestanding -Wno-cast-qual -Wno-format-extra-args -Wno-error=undef -I$K/common/inc -I$K -I$K/nvidia-modeset -Ibuild/nvidia-kpi/out -I$NV/src/common/sdk/nvidia/inc -I$NV/src/common/inc"
ok=0; bad=0; : > $O/errors.txt
for f in nvidia-modeset-linux nv-kthread-q; do
    if gcc $FL -c $K/nvidia-modeset/$f.c -o $O/$f.o 2> $O/$f.err; then ok=$((ok+1)); else bad=$((bad+1)); fi
    grep -E "error|fatal" $O/$f.err | sed "s|^[^ ]*/||;s|^|$f: |" >> $O/errors.txt
done
echo "compilan: $ok  con errores: $bad"
head -${TOP:-30} $O/errors.txt
