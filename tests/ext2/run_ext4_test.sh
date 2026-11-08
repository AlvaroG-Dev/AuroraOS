#!/bin/bash
# E8: lectura de ext4 (extents, 64bit, flex_bg, htree, metadata_csum) en el guest.
# Compara sha256 y listados contra el host. FEAT="..." cambia las features.
#   FEAT='^64bit,^metadata_csum' tests/ext2/run_ext4_test.sh
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-ext4; IMG=$W/e.img; LOG=/tmp/ext2-ext4.log
rm -rf $W; mkdir -p $W/src/dir $W/src/many
head -c 5000000 /dev/urandom > $W/src/big
head -c 777 /dev/urandom > $W/src/small
echo corto > $W/src/c.txt
# fichero con muchos extents (arbol de profundidad >= 1): trozos dispersos
: > $W/src/frag; for i in $(seq 0 299); do dd if=/dev/urandom of=$W/src/frag bs=4096 seek=$((i*3)) count=1 conv=notrunc 2>/dev/null; done
truncate -s $((900*4096)) $W/src/frag
: > $W/src/sparse; dd if=/dev/urandom of=$W/src/sparse bs=1M seek=40 count=1 2>/dev/null
for i in $(seq 1 3000); do echo $i > $W/src/many/archivo_con_nombre_largo_$i.txt; done
ln -s c.txt $W/src/fastlink; ln -s "$(printf 'x%.0s' $(seq 1 120))/destino" $W/src/slowlink
mkdir -p $W/src/dir/a/b/c; echo hondo > $W/src/dir/a/b/c/f
mke2fs -q -F -t ext4 -b ${BS:-4096} -O "${FEAT:-}" -L EXT4TEST -d $W/src $IMG 64M 2>&1 | grep -v "^$" | head -3
dumpe2fs -h $IMG 2>/dev/null | grep -i "features\|Inode size"
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-400}" 'sleep 30' \
 'umount /data; mount -t ext4 -o ro /dev/sda /data; echo @@MOUNT rc=$?' \
 'cd /data; for f in big small c.txt frag sparse dir/a/b/c/f; do sha256sum $f; done | sed "s/^/@@SHA /"' \
 'cd /data; echo "@@N $(ls -f many | wc -l)"; echo "@@L $(readlink fastlink) $(readlink slowlink | wc -c)"; stat -c "@@ST %n %s" sparse frag big; cat many/archivo_con_nombre_largo_2999.txt | sed "s/^/@@M /"; touch /data/x 2>&1 | sed "s/^/@@RO /"' \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@\|\[EXT2\]" | grep -av "bash -c" | cut -c1-160
echo "-- host:"; (cd $W/src && for f in big small c.txt frag sparse dir/a/b/c/f; do sha256sum $f; done; echo "N $(( $(ls many | wc -l) + 2 ))"; stat -c "ST %n %s" sparse frag big)
