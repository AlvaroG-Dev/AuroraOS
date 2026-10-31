#!/bin/bash
# Repro: arranque UEFI solo desde un USB (aurora.iso volcada), sin CD.
# uso: usbrepro.sh [xhci-device] [extra qemu args...]
cd /home/alvaro/aurora-os-copy
XH=${1:-qemu-xhci}
cp aurora.iso /tmp/usbdisk.img
rm -f /tmp/k.log
python3 -I scripts/qemu-drive.py -m ${MEM:-2G} -t 400 --log /tmp/k.log --no-cdrom --no-build \
  --extra=-enable-kvm --extra=-cpu --extra=host \
  --extra=-drive --extra=if=none,id=u,file=/tmp/usbdisk.img,format=raw \
  --extra=-device --extra=$XH \
  --extra=-device --extra=usb-storage,drive=u,bootindex=0 \
  --done "KLOG] log en vivo" 2>&1 | tail -5
grep -a -E "PART\]|FAT32\]|KLOG\]|INIT\] Montado|xhci|XHCI|usb|blk" /tmp/k.log | head -60
