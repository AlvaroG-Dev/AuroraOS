#!/bin/bash
# Mide el arranque con la pila NVIDIA simulada (nvidia.fake=1) en QEMU+KVM.
# Uso: build/nvboot.sh [segundos]   -> log en /tmp/nvboot.log
cd "$(dirname "$0")/.."
T=${1:-200}
cp bootloader/aurora.conf /tmp/aurora.conf.bak
trap 'cp /tmp/aurora.conf.bak bootloader/aurora.conf' EXIT
sed -i 's|^cmdline=.*|cmdline=lkpi.load=linuxkpi,lkpi_drm,nvidia,nvidia-modeset,nvidia-drm nvidia.fake=1 klog.file=off|' bootloader/aurora.conf
SKIP_ENV=1 make iso >/tmp/nvboot-build.log 2>&1 || { tail -30 /tmp/nvboot-build.log; exit 1; }
cp /tmp/aurora.conf.bak bootloader/aurora.conf
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
rm -f /tmp/nvboot.log
timeout "$T" qemu-system-x86_64 -enable-kvm -cpu host \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -m 2G -no-reboot -smp 2 -cdrom aurora.iso -display none \
  -serial file:/tmp/nvboot.log || true
wc -l /tmp/nvboot.log
