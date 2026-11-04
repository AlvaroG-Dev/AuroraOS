#!/bin/bash
# blkid, lsblk, findfs, uuidgen, mountpoint y utilidades de e2fsprogs en el guest.
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-tools; IMG=$W/t.img; LOG=/tmp/ext2-tools.log; mkdir -p $W/src
echo hola > $W/src/hola.txt
rm -f $IMG; mke2fs -q -F -t ext2 -b 4096 -L AURORADATA -d $W/src $IMG 64M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-300}" 'sleep 30' \
 'echo "@@BLKID $(/usr/sbin/blkid /dev/sda)"; echo "@@BLKID_L $(/usr/sbin/blkid -s LABEL -o value /dev/sda)"; echo "@@FINDFS $(/usr/sbin/findfs LABEL=AURORADATA)"' \
 'echo "@@UUID $(/usr/sbin/uuidgen)"; /usr/sbin/lsblk | sed "s/^/@@LSBLK /"; /usr/sbin/mountpoint /data | sed "s/^/@@MP /"' \
 'umount /data; /usr/sbin/resize2fs -P /dev/sda 2>&1 | sed "s/^/@@RSZ /"; /usr/sbin/e2image /dev/sda /tmp/a.e2i 2>&1 | sed "s/^/@@E2I /"; ls -la /tmp/a.e2i | sed "s/^/@@E2I /"' \
 'mount -t ext2 /dev/sda /data; lsattr /data/hola.txt 2>&1 | sed "s/^/@@LSATTR /"; chattr +A /data/hola.txt 2>&1 | sed "s/^/@@CHATTR /"; /usr/sbin/e2freefrag /dev/sda 2>&1 | head -3 | sed "s/^/@@FRAG /"; echo @@END' \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@" | grep -av "bash -c" | cut -c1-150
