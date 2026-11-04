#!/bin/bash
# Mide ls -f / stat / ls -l en un directorio de 5000 ficheros (caché de nombres).
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-dir; IMG=$W/d.img; LOG=/tmp/ext2-dir.log; mkdir -p $W/seed/big
[ -f $W/seed/big/f4999 ] || for i in $(seq 0 4999); do echo $i > $W/seed/big/f$i; done
rm -f $IMG; mke2fs -q -F -t ext2 -b 4096 -d $W/seed $IMG 64M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-420}" 'sleep 40' \
 'umount /data; mount -t ext2 -o ro /dev/sda /data; echo @@M rc=$?' \
 's=$(date +%s); ls -f /data/big | wc -l | sed "s/^/@@LSF n=/"; echo @@LSF_T $(( $(date +%s)-s ))s' \
 's=$(date +%s); ls -l /data/big | wc -l | sed "s/^/@@LSL n=/"; echo @@LSL_T $(( $(date +%s)-s ))s' \
 'cat /data/big/f4999 | sed "s/^/@@CAT /"; ls /data/big/nope 2>&1 | sed "s/^/@@NOPE /"' \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@" | grep -av "bash -c" | cut -c1-140
