#!/bin/bash
# scripts/nvidia-src-patch.sh — aplica kernel/linuxkpi/patches/nvidia-src/<fichero>.patch
# al arbol src/nvidia de open-gpu-kernel-modules (el nucleo RM, nv-kernel.o).
# Idempotente: si el parche ya esta aplicado no hace nada. Si aplica alguno borra
# nv-kernel.o para que `make nvidia` lo recompile.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
T=build/linuxkpi_src/nvidia-open/src/nvidia
for p in kernel/linuxkpi/patches/nvidia-src/*.patch; do
    [ -f "$p" ] || continue
    b=$(basename "$p" .patch); t=$(find $T -name "$b" -not -path '*/_out/*' | head -1)
    [ -n "$t" ] || { echo "nvidia-src-patch: no encuentro $b" >&2; exit 1; }
    if patch -R --dry-run -s "$t" "$p" >/dev/null 2>&1; then continue; fi
    patch -s "$t" "$p" || { echo "nvidia-src-patch: $p no aplica" >&2; exit 1; }
    rm -f $T/_out/Linux_x86_64/nv-kernel.o
    echo "[NVIDIA] parche aplicado: $b"
done
