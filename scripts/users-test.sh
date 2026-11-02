#!/bin/bash
# [U3] Prueba de login/su/passwd/useradd/sudo en QEMU (WITH_NVIDIA=0).
set -u
cd "$(dirname "$0")/.."
IMG=/tmp/users-test-fat.img
rm -f "$IMG"; truncate -s 64M "$IMG"; mkfs.vfat -F 32 "$IMG" >/dev/null
cp scripts/users-selftest.sh sysroot/usr/bin/users-selftest.sh
printf 'bash /usr/bin/users-selftest.sh\n' > sysroot/system/boot-tests
make iso WITH_NVIDIA=0 >/tmp/users-test-build.log 2>&1 || { tail -20 /tmp/users-test-build.log; exit 1; }
rm -f sysroot/system/boot-tests sysroot/usr/bin/users-selftest.sh
LOG=/tmp/users-test.log; rm -f "$LOG"
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
timeout 420 qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -m 2G -no-reboot -smp 2 -cdrom aurora.iso -display none \
  -drive file="$IMG",format=raw,if=none,id=d0 -device ich9-ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 \
  -serial file:"$LOG" >/dev/null 2>&1 &
Q=$!
for _ in $(seq 1 400); do grep -aq "users\] RESULT" "$LOG" 2>/dev/null && break; sleep 1; done
sleep 2; kill $Q 2>/dev/null; wait $Q 2>/dev/null
grep -a "\[users\]" "$LOG" | cut -c1-220
grep -aq "users\] RESULT pass=[0-9]* fail=0" "$LOG"
