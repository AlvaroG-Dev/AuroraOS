#!/bin/bash
# E9.1: reproducción del journal jbd2 al montar rw. Imagen con dos transacciones sin
# reproducir (la 2ª escribe B y revoca A); debugfs 'jr' en el host es el oráculo.
#   FEAT='^metadata_csum' (por defecto) o FEAT='metadata_csum'
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-journal; IMG=$W/j.img; LOG=/tmp/ext2-journal.log
rm -rf $W; mkdir -p $W/src
head -c 16384 /dev/urandom > $W/src/t
mke2fs -q -F -t ext4 -O "${FEAT:-^metadata_csum}" -L JT -d $W/src $IMG 16M
A=$(debugfs -R "bmap t 0" $IMG 2>/dev/null); B=$(debugfs -R "bmap t 1" $IMG 2>/dev/null)
head -c 4096 /dev/zero | tr '\0' a > $W/pa; head -c 4096 /dev/zero | tr '\0' b > $W/pb; head -c 4096 /dev/zero | tr '\0' c > $W/pc
printf 'jo\njw -b %s %s/pa\njw -b %s %s/pb\njc\njo\njw -b %s %s/pc\njw -r %s\njc\n' $A $W $B $W $B $W $A > $W/cmds
debugfs -w -f $W/cmds $IMG >/dev/null 2>&1
dumpe2fs -h $IMG 2>/dev/null | grep -c needs_recovery | sed 's/^/needs_recovery antes: /'
cp $IMG $W/oracle.img; debugfs -w -R jr $W/oracle.img >/dev/null 2>&1
ora() { dd if=$W/oracle.img bs=4096 skip=$1 count=1 2>/dev/null | sha256sum | cut -c1-16; }
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL QEMU_MEM=${QEMU_MEM:-4G}
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-500}" 'sleep 30' \
 'umount /data; mount -t ext4 -o rw /dev/sda /data; echo @@MOUNT rc=$?' \
 'umount /data; echo @@UMOUNT rc=$?' \
 'for b in '$A' '$B'; do echo "@@BLK $b $(dd if=/dev/sda bs=4096 skip=$b count=1 2>/dev/null | sha256sum | cut -c1-16)"; done' \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@\|\[EXT2\]" | grep -av "bash -c" | cut -c1-160
echo "-- oráculo: A=$A $(ora $A)  B=$B $(ora $B)"
echo "-- needs_recovery después: $(dumpe2fs -h $IMG 2>/dev/null | grep -c needs_recovery)"
e2fsck -fn $IMG 2>&1 | tail -3
