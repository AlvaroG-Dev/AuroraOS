#!/bin/bash
# scripts/nvidia-drm-cc.sh — compila kernel-open/nvidia-drm contra LinuxKPI + lkpi_drm
# (Plan_NVIDIA.md: modeset por nvidia-drm sobre nvkms). Requiere nvidia-kpi-cc.sh antes.
# Uso: scripts/nvidia-drm-cc.sh [fichero ...]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
NV=build/linuxkpi_src/nvidia-open; K=build/nvidia-kpi/ko; O=build/nvidia-kpi/drm/obj
mkdir -p "$O"
cat > build/nvidia-kpi/nvdrm-flags.mk <<'MK'
include kernel/linuxkpi/flags.mk
include kernel/linuxkpi/drivers/lkpi_drm_cflags.mk
show:
	@echo $(LKPI_CFLAGS) $(LKPI_DRM_CFLAGS)
MK
FL="$(make -s -f build/nvidia-kpi/nvdrm-flags.mk show) -DNVRM -DKBUILD_MODNAME=\"nvidia_drm\" -DNV_VERSION_STRING=\"615.78.08\" -DNV_KERNEL_INTERFACE_LAYER -DNV_SPECTRE_V2=0 -DNVIDIA_UNDEF_LEGACY_BIT_MACROS -UDEBUG -U_DEBUG -DNDEBUG -DNV_BUILD_MODULE_INSTANCES=0 -include linux/mm.h -ffreestanding -Wno-cast-qual -Wno-format-extra-args -Wno-error=undef -I$K/common/inc -I$K -I$K/nvidia-drm -Ibuild/nvidia-kpi/out -I$NV/src/common/sdk/nvidia/inc -I$NV/src/common/inc -I$NV/src/nvidia-modeset/kapi/interface -I$NV/src/nvidia-modeset/include -I$NV/src/common/unix/common/inc -I$NV/src/common/modeset/hdmipacket -I$NV/src/common/modeset/timing"
SRCS=${*:-$(sed -n 's|^NVIDIA_DRM_SOURCES += nvidia-drm/\(.*\)\.c$|\1|p' $K/nvidia-drm/nvidia-drm-sources.mk) nvidia-drm-linux}
ok=0; bad=0; : > $O/errors.txt
for f in $SRCS; do
    f=${f%.c}
    rm -f $O/$f.o
    if gcc $FL -c $K/nvidia-drm/$f.c -o $O/$f.o 2> $O/$f.err; then ok=$((ok+1)); else bad=$((bad+1)); fi
    grep -E "error|fatal" $O/$f.err | sed "s|^[^ ]*/||;s|^|$f: |" >> $O/errors.txt
done
echo "compilan: $ok  con errores: $bad  ($(grep -c . $O/errors.txt) mensajes)"
sed 's/^[^:]*: //;s/^[0-9]*:[0-9]*: //' $O/errors.txt | sort | uniq -c | sort -rn | head -${TOP:-25}
[ $bad -eq 0 ] || exit 1
