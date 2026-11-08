#!/bin/bash
# E7.4: imagen AURORADATA sucia -> monta ro -> aurora-fsck repara y vuelve a rw.
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-autofsck; IMG=$W/a.img; LOG=/tmp/ext2-autofsck.log; mkdir -p $W/src
echo hola > $W/src/hola.txt
rm -f $IMG; mke2fs -q -F -t ext2 -b 4096 -L AURORADATA -d $W/src $IMG 64M || exit 2
debugfs -w -R 'ssv state 0' $IMG >/dev/null 2>&1
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-300}" 'sleep 30' \
 'umount /data; mount -t ext2 -o ro /dev/sda /data; echo @@PRE rc=$?; touch /data/x 2>&1 | sed "s/^/@@PRE_TOUCH /"' \
 '/usr/sbin/aurora-fsck 2>&1 | sed "s/^/@@FSCK /"' \
 'touch /data/escrito && echo @@POST_RW_OK; cat /data/hola.txt | sed "s/^/@@HOLA /"; umount /data; echo @@UM $?' \
 'umount /data; mount -t ext2 /dev/sda /data; echo hola > /data/ab; sleep 600 < /data/ab & sleep 1; /usr/sbin/aurora-fsck 2>&1 | sed "s/^/@@LIVE /"; echo @@LIVE_END; cat /data/ab; touch /data/vivo && echo @@LIVE_RW_OK' \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@" | grep -av "bash -c" | cut -c1-140
echo "-- host e2fsck:"; e2fsck -fn $IMG | tail -2
