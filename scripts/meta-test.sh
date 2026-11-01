#!/bin/bash
# [U2] Prueba de persistencia de modo/uid/gid en /data (FAT32) en QEMU.
# Arranca dos veces con el mismo disco FAT32 y ejecuta /usr/bin/meta_test.
# Uso: scripts/meta-test.sh        (requiere haber hecho make iso WITH_NVIDIA=0)
set -u
cd "$(dirname "$0")/.."
IMG=/tmp/meta-test-fat.img
rm -f "$IMG"
truncate -s 64M "$IMG"
mkfs.vfat -F 32 "$IMG" >/dev/null
printf '/usr/bin/meta_test\n' > sysroot/system/boot-tests
make iso WITH_NVIDIA=0 >/tmp/meta-test-build.log 2>&1 || { tail -20 /tmp/meta-test-build.log; exit 1; }
rm -f sysroot/system/boot-tests
fail=0
for phase in 1 2; do
  LOG=/tmp/meta-test-$phase.log
  rm -f "$LOG"
  cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
  timeout 240 qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=OVMF_VARS.fd \
    -m 2G -no-reboot -smp 2 -cdrom aurora.iso -display none \
    -drive file="$IMG",format=raw,if=none,id=d0 -device ich9-ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 \
    -serial file:"$LOG" >/dev/null 2>&1 &
  Q=$!
  for _ in $(seq 1 200); do
    grep -aq "meta_test\] RESULT\|meta_test\] SKIP" "$LOG" 2>/dev/null && break
    sleep 1
  done
  sleep 3; kill $Q 2>/dev/null; wait $Q 2>/dev/null
  echo "== fase $phase =="; grep -a "meta_test\]\|FAT32\] metadatos\|FAT32\] sin .aurora\|Montado .* en /data" "$LOG" | cut -c1-160
  grep -aq "meta_test\] RESULT pass=[0-9]* fail=0" "$LOG" || fail=1
done
exit $fail
