#!/bin/bash
# Prueba de /data en un USB (klog.file) en QEMU+UEFI. Dos escenarios:
#   A) arranque solo desde el USB (sin CD), como en un PC real.
#   B) USB enchufado en caliente tras ~25 s: debe salir el resumen [DIAG] y
#      luego montarse /data por el reintento periodico.
# Uso: scripts/usb-data-test.sh [--no-build]    (requiere KVM o paciencia)
set -u
cd "$(dirname "$0")/.."
NOBUILD=""
[ "${1:-}" = "--no-build" ] && NOBUILD="--no-build"
LOG=/tmp/usb-data-test.log
KVM="--extra=-enable-kvm --extra=-cpu --extra=host"
[ -w /dev/kvm ] || KVM=""
fail=0

run() { # nombre, marcador, args...
  local name=$1 mark=$2; shift 2
  rm -f "$LOG"
  python3 -I scripts/qemu-drive.py -m 2G -t 300 --log "$LOG" $KVM \
    --cmdline "klog.file=/data/aurora.log" --done "$mark" "$@" >/tmp/usb-data-test.out 2>&1
  if grep -aq "$mark" "$LOG"; then echo "PASS $name"; else
    echo "FAIL $name"; fail=1; grep -a -E "DIAG|PART|FAT32|KLOG|USB-STG" "$LOG" | tail -20; fi
}

# B primero: construye la ISO nueva (con el CD de arranque) y el USB aparece tarde.
cp aurora.iso /tmp/usbdisk.img 2>/dev/null || true
run "B: USB en caliente (diag + reintento)" "KLOG] log en vivo" $NOBUILD \
  --extra=-drive --extra=if=none,id=u,file=/tmp/usbdisk.img,format=raw \
  --extra=-device --extra=qemu-xhci,id=xh \
  --step "[DIAG] esperando /data=device_add usb-storage,drive=u,bus=xh.0"
grep -aq "\[DIAG\] esperando /data" "$LOG" && echo "PASS B: resumen [DIAG] presente" || { echo "FAIL B: sin [DIAG]"; fail=1; }

# A: arranque solo desde el USB.
cp aurora.iso /tmp/usbdisk.img
run "A: UEFI solo-USB (sin CD)" "KLOG] log en vivo" --no-build --no-cdrom \
  --extra=-drive --extra=if=none,id=u,file=/tmp/usbdisk.img,format=raw \
  --extra=-device --extra=qemu-xhci \
  --extra=-device --extra=usb-storage,drive=u,bootindex=0
exit $fail
