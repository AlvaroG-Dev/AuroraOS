#!/bin/bash
# mmap(MAP_SHARED) escribible sobre ext2: pasos de user/apps/ext2_posix --mmap en
# AuroraOS, y los ficheros resultantes deben ser identicos a los que deja Linux.
set -u
cd "$(dirname "$0")/../.."
BS=${BS:-1024}; TAG=mm_$BS; W=build/ext2-write; mkdir -p $W; IMG=$W/$TAG.img; LOG=/tmp/ext2-$TAG.log
rm -f $IMG; mke2fs -q -F -t ext2 -b $BS -I 256 $IMG 64M || exit 2
gcc -static -O1 -w -o sysroot/usr/bin/ext2_posix user/apps/ext2_posix/main.c || exit 2
rm -rf /tmp/mm_host; mkdir -p /tmp/mm_host; sysroot/usr/bin/ext2_posix --mmap /tmp/mm_host/mm > $W/mm_host.txt
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
trap 'rm -f sysroot/usr/bin/ext2_posix' EXIT
bash scripts/qemu-boot-test.sh -t "${T:-600}" \
 "sleep 40" \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?' \
 'echo @@MM; /usr/bin/ext2_posix --mmap /data/mm; echo @@END' \
 'umount /data; echo @@UM rc=$?' 'echo @@DONE' 'power off' >/dev/null 2>&1
sec() { tr -d '\r' < $LOG | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -E '^mmap '; }
sec MM > $W/$TAG.guest.txt
if diff $W/mm_host.txt $W/$TAG.guest.txt > $W/$TAG.diff; then echo "pasos: IGUALES a Linux ($(grep -c OK$ $W/$TAG.guest.txt) OK)"; else echo "pasos: DIFERENCIAS"; cat $W/$TAG.diff; fi
e2fsck -fn $IMG > $W/$TAG.fsck 2>&1; rc=$?; echo "e2fsck -fn rc=$rc"; [ $rc -ne 0 ] && head -12 $W/$TAG.fsck
rm -rf $W/$TAG.out; mkdir -p $W/$TAG.out; debugfs -R "rdump /mm $W/$TAG.out" $IMG >/dev/null 2>&1
for f in m1 m2 m3; do
  a=$(sha256sum < /tmp/mm_host/mm/$f | cut -c1-16); b=$(sha256sum < $W/$TAG.out/mm/$f 2>/dev/null | cut -c1-16)
  [ "$a" = "$b" ] && echo "$f: contenido IGUAL a Linux ($a)" || echo "$f: DISTINTO linux=$a ext2=$b"
done
