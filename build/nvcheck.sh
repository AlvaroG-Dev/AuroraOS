#!/bin/bash
# Arranque QEMU+KVM con nvidia.fake=1 y resumen del volumen de log / hitos.
cd "$(dirname "$0")/.."
build/nvboot.sh ${1:-90} 2>&1 | tail -2
L=/tmp/nvboot.log
echo "exports=$(grep -ac 'DEBUG \[KUR\] export' $L) smpdump=$(grep -ac 'BSP-after' $L) lineas=$(wc -l < $L) panics=$(grep -ac 'KERNEL PANIC' $L)"
grep -a -n "WM listo\|nvidia-drm.*cargado\|No NVIDIA GPUs" $L | cut -c1-110
