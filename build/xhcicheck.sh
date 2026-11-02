#!/bin/bash
# Arranca aurora.iso (ya construida) con xHCI + teclado/raton USB y resume la enumeracion.
cd "$(dirname "$0")/.."
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
rm -f /tmp/xhcichk.log /tmp/xhcichk.sock
(
  sleep ${1:-40}
  python3 -I - <<'PY'
import socket, time
s = socket.socket(socket.AF_UNIX)
for _ in range(20):
    try:
        s.connect('/tmp/xhcichk.sock'); break
    except Exception:
        time.sleep(0.5)
for i in range(30):
    s.send(b'mouse_move 15 7\n'); time.sleep(0.02)
time.sleep(1)
PY
) &
timeout $((${1:-40}+25)) qemu-system-x86_64 -enable-kvm -cpu host \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -m 2G -no-reboot -smp 2 -cdrom aurora.iso -display none \
  -device qemu-xhci -device usb-kbd -device usb-mouse \
  -monitor unix:/tmp/xhcichk.sock,server,nowait \
  -serial file:/tmp/xhcichk.log || true
grep -a "UPROXY\] new device\|usbhid-.*kthread\|xhci.aur\] carga\|PANIC\|WM listo" /tmp/xhcichk.log | cut -c1-140 | head -20
