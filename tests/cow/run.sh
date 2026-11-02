#!/bin/bash
# Arranca en QEMU (-smp 4) un initrd minimo cuyo /usr/bin/init es cow_test.c y
# espera "COW-TEST: DONE". Uso: tests/cow/run.sh [tiempo_max_segundos]
# Requiere: kernel/kernel.elf (make -C kernel), bootloader/efi/BOOTX64.EFI
# (make bootloader-efi), gcc, mtools, OVMF y qemu-system-x86_64.
set -e
cd "$(dirname "$0")/../.."
TMO=${1:-600}
W=${COW_WORK:-/tmp/cow_run}
rm -rf "$W"; mkdir -p "$W/root/usr/bin"
gcc -static -nostdlib -fno-stack-protector -fno-pic -no-pie -O1 \
    -o "$W/root/usr/bin/init" tests/cow/cow_test.c
tar --format=ustar --owner=0 --group=0 -cf "$W/initrd.tar" -C "$W/root" .
IMG="$W/efi.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mkfs.fat -F 16 -n EFIBOOT "$IMG" >/dev/null
mmd -i "$IMG" ::/EFI ::/EFI/BOOT ::/aurora
mcopy -i "$IMG" bootloader/efi/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" kernel/kernel.elf ::/aurora/kernel.elf
mcopy -i "$IMG" "$W/initrd.tar" ::/aurora/initrd.tar
mcopy -i "$IMG" bootloader/aurora.conf ::/aurora/aurora.conf
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
ACCEL=${COW_ACCEL:--accel tcg}
timeout "$TMO" qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$W/vars.fd" \
  -m ${COW_MEM:-1024} -smp 4 $ACCEL -machine q35 -display none -no-reboot \
  -drive file="$IMG",format=raw,if=ide \
  -serial file:"$W/serial.log" &
Q=$!
for i in $(seq 1 "$TMO"); do
  sleep 1
  if grep -q "COW-TEST: DONE" "$W/serial.log" 2>/dev/null; then break; fi
  kill -0 $Q 2>/dev/null || break
done
kill $Q 2>/dev/null || true
grep -a "COW-TEST\|PANIC\|\[PF\]" "$W/serial.log" | head -80
grep -aq "COW-TEST: PASS todo" "$W/serial.log"
