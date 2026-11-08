#!/bin/bash
# Cierre de E4/E5: lista de huérfanos en disco, lost+found automático,
# ext2_check() (contadores corregidos), s_mnt_count y fstab/mount -a.
# Oráculo en el host: dumpe2fs, e2fsck, debugfs.
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-e4rest; mkdir -p $W
A=$W/a.img; B=$W/b.img; LOG=${LOG:-/tmp/ext2-e4rest.log}
rm -f $A $B
mke2fs -q -F -t ext2 -b 4096 -L CASO_A $A 64M || exit 2
mke2fs -q -F -t ext2 -b 1024 -L CASO_B $B 8M || exit 2
# A: sin lost+found y con contadores de grupo falsos (como tras un corte)
debugfs -w -R 'rmdir lost+found' $A >/dev/null 2>&1
debugfs -w -R 'set_bg 0 free_blocks_count 7' $A >/dev/null 2>&1
debugfs -w -R 'set_bg 0 checksum calc' $A >/dev/null 2>&1
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$A,if=none,id=e2,format=raw -drive file=$B,if=none,id=e3,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0 -device ide-hd,drive=e3,bus=ahci0.1"
mkdir -p sysroot/etc sysroot/mnt/b
printf '/dev/sdb /mnt/b ext2 defaults,noatime 0 0\n' > sysroot/etc/fstab
bash scripts/qemu-boot-test.sh -t "${T:-170}" \
 'sleep 40' \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?; ls -ld /data/lost+found; df /data | tail -1' \
 'echo @@FSTAB; grep -a " /mnt/b " /proc/mounts; echo hola > /mnt/b/f && cat /mnt/b/f' \
 'cd /data; seq 1 100000 > o1; seq 1 100000 > o2; exec 3<o1; exec 4<o2; rm o1 o2; exec 3<&-; sleep 12; echo @@ORPH; sleep 600' \
 >/dev/null 2>&1
cp etc/fstab sysroot/etc/fstab
fail=0
tr -d '\r' < "$LOG" | grep -a -E '@@|lost\+found|\[EXT2\]|^/dev|hola' | grep -a -v '^\[ *[0-9.]*\] DEBUG' | cut -c1-170
grep -a -q '@@ORPH' "$LOG" || { echo "FALLO: no llegó a @@ORPH"; fail=1; }
echo "-- host A:"
dumpe2fs -h $A 2>/dev/null | grep -E 'Mount count|First orphan|Filesystem state'
debugfs -R 'ls -l /' $A 2>/dev/null | grep lost
echo "-- e2fsck -fn (esperado: limpiar 1 huérfano):"
e2fsck -fn $A 2>&1 | grep -v '^Pass' | head -8
e2fsck -fy $A >/dev/null 2>&1
e2fsck -fn $A >/dev/null 2>&1; rc=$?; echo "tras -fy, -fn rc=$rc"; [ $rc -ne 0 ] && fail=1
echo "-- host B:"; e2fsck -fn $B 2>&1 | tail -1; debugfs -R 'cat /f' $B 2>/dev/null
exit $fail
