#!/bin/bash
# Mide la velocidad de lectura de ext2 en frío (dd con distintos tamaños) y
# comprueba el contenido con sha256. Uso: TAG=antes|despues tests/ext2/run_readspeed_test.sh
set -u
cd "$(dirname "$0")/../.."
TAG=${TAG:-medida}; W=build/ext2-speed; mkdir -p $W
IMG=$W/s.img; LOG=/tmp/ext2-speed-$TAG.log
if [ ! -f $W/src/small ]; then
  mkdir -p $W/src; for f in big1:32 big2:16 big3:16 small:6; do head -c $(( ${f#*:} * 1048576 )) /dev/urandom > $W/src/${f%:*}; done
fi
rm -f $IMG; mke2fs -q -F -t ext2 -b ${BS:-4096} -d $W/src $IMG 128M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-420}" 'sleep 40' \
 'umount /data; mount -t ext2 -o ro /dev/sda /data; echo @@M rc=$?' \
 'sha256sum /data/small | sed "s/^/@@SHA /"' \
 'dd if=/data/big1 of=/dev/null bs=64k 2>&1 | tail -1 | sed "s/^/@@R64K /"' \
 'dd if=/data/big2 of=/dev/null bs=4k 2>&1 | tail -1 | sed "s/^/@@R4K /"' \
 'dd if=/data/big3 of=/dev/null bs=1M 2>&1 | tail -1 | sed "s/^/@@R1M /"' \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@R\|@@M\|@@DONE" | cut -c1-140
echo "-- host:"; (cd $W/src && sha256sum small | cut -c1-16,65-)
tr -d '\r' < $LOG | grep -a "@@SHA" | cut -c1-16,23-90
