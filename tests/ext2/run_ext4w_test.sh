#!/bin/bash
# E9.2/E9.3: escritura en ext4 (extents, 64bit, flex_bg). El guest escribe, trunca,
# borra y crea; el host valida con e2fsck -fn y compara sha256 con debugfs.
#   FEAT='^64bit' / BS=1024 cambian la imagen.
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-ext4w; IMG=$W/e.img; LOG=/tmp/ext2-ext4w.log
rm -rf $W; mkdir -p $W/src/dir
head -c 5000000 /dev/urandom > $W/src/big
head -c 777 /dev/urandom > $W/src/small
: > $W/src/frag; for i in $(seq 0 299); do dd if=/dev/urandom of=$W/src/frag bs=4096 seek=$((i*3)) count=1 conv=notrunc 2>/dev/null; done
: > $W/src/gone; head -c 300000 /dev/urandom > $W/src/gone
mkdir -p $W/src/many; for i in $(seq 1 3000); do echo $i > $W/src/many/archivo_con_nombre_largo_$i.txt; done
head -c 409600 /dev/zero > $W/src/ufile
mke2fs -q -F -t ext4 -b ${BS:-4096} -O "${FEAT:-^metadata_csum}" -L EXT4W -d $W/src $IMG 64M 2>&1 | head -3
debugfs -w -R "fallocate ufile 0 99" $IMG >/dev/null 2>&1
dumpe2fs -h $IMG 2>/dev/null | grep -i "features"
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL QEMU_MEM=${QEMU_MEM:-4G}
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-600}" 'sleep 30' \
 'umount /data; mount -t ext4 -o rw /dev/sda /data; echo @@MOUNT rc=$?' \
 'cd /data; for i in $(seq 0 399); do dd if=/dev/urandom of=fragw bs=4096 seek=$((i*3)) count=1 conv=notrunc 2>/dev/null; done; sha256sum fragw | sed "s/^/@@SHA /"' \
 'cd /data; head -c 3000000 /dev/urandom >> big; sha256sum big | sed "s/^/@@SHA /"; dd if=/dev/urandom of=frag bs=4096 seek=100 count=50 conv=notrunc 2>/dev/null; sha256sum frag | sed "s/^/@@SHA /"' \
 'cd /data; head -c 10000 /dev/urandom > dir/nuevo; sha256sum dir/nuevo | sed "s/^/@@SHA /"; dd if=/dev/null of=gone bs=1 seek=123457 count=0 2>/dev/null; sha256sum gone | sed "s/^/@@SHA /"; rm small' \
 'cd /data; mkdir -p n/a/b; for i in $(seq 1 200); do echo $i > n/a/b/f$i; done; rm n/a/b/f1*; echo "@@N $(ls n/a/b | wc -l)"' \
 'cd /data; touch many/nuevo_a many/nuevo_b; rm many/archivo_con_nombre_largo_5.txt; mv many/nuevo_a many/renombrado; mkdir many/sub; mv many/sub n/sub2; echo @@MANY ok' \
 'cd /data; dd if=/dev/urandom of=ufile bs=4096 seek=50 count=1 conv=notrunc 2>/dev/null; sha256sum ufile | sed "s/^/@@SHA /"' \
 'cd /data; dd if=/dev/urandom of=ufile bs=4096 seek=0 count=2 conv=notrunc 2>/dev/null; dd if=/dev/urandom of=ufile bs=100 seek=1 count=3 conv=notrunc 2>/dev/null; sha256sum ufile | sed "s/^/@@SHA /"' \
 'cd /data; python3 -c "f=open(\"deep\",\"wb\"); [(f.seek(i*8192), f.write(bytes([i%251])*4096)) for i in range(2400)]; f.close()"; sha256sum deep | sed "s/^/@@SHA /"' \
 'cd /data; python3 -c "import os;os.truncate(\"deep\",8192*900+100)"; sha256sum deep | sed "s/^/@@SHA /"; echo @@DEEPDONE' \
 'cd /data; cp big big2; sha256sum big2 | sed "s/^/@@SHA /"; : > frag; rm big; sync; echo @@SYNC rc=$?' \
 'umount /data; echo @@UMOUNT rc=$?; echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@\|\[EXT2\]" | grep -av "bash -c" | cut -c1-160
echo "-- host:"
e2fsck -fn $IMG 2>&1 | tail -6; echo "fsck_rc=${PIPESTATUS[0]}"
for f in fragw frag gone big2 dir/nuevo ufile deep; do printf '%s ' $f; debugfs -R "cat $f" $IMG 2>/dev/null | sha256sum | cut -c1-64; done
debugfs -R "ls n/a/b" $IMG 2>/dev/null | wc -w
