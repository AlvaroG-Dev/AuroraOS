#!/bin/bash
# FS grande ya poblado con mke2fs -d (como la particion de datos de la ISO):
# crear ficheros y directorios nuevos, sync, y comprobar con e2fsck que no hay
# bloques reclamados dos veces.
set -u
cd "$(dirname "$0")/../.."
SRC=${SRC:-build/usbdata.img}; W=build/ext2-bigfs; mkdir -p $W; IMG=$W/b.img
[ -f $SRC ] || { echo "falta $SRC (make iso USB_DATA_FS=ext2)"; exit 2; }
cp --sparse=always $SRC $IMG
LOG=/tmp/ext2-bigfs.log; export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-500}" 'sleep 30' \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?' \
 'mkdir -p /data/nw; cd /data/nw; for i in $(seq 1 150); do head -c $((RANDOM*2+1000)) /dev/urandom > o$i.o; done; echo @@FILES' \
 'cd /data/nw; for i in $(seq 1 40); do mkdir d$i; for j in 1 2 3 4 5 6; do echo $i$j > d$i/f$j; done; done; echo @@DIRS' \
 'sync; echo @@SYNCED' 'echo @@DONE' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@" | grep -av "bash -c" | cut -c1-100
e2fsck -fn $IMG 2>&1 | tail -12
