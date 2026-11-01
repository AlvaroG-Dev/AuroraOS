#!/bin/bash
# E3.8: casos POSIX (user/apps/ext2_posix) sobre ext2 en AuroraOS contra la
# salida del mismo binario en Linux (oraculo), + noexec/nodev/sync y e2fsck.
set -u
cd "$(dirname "$0")/../.."
BS=${BS:-1024}; TAG=pc_$BS; W=build/ext2-write; mkdir -p $W; IMG=$W/$TAG.img; LOG=/tmp/ext2-$TAG.log
rm -f $IMG; mke2fs -q -F -t ext2 -b $BS -I 256 $IMG 128M || exit 2
gcc -static -O1 -w -o sysroot/usr/bin/ext2_posix user/apps/ext2_posix/main.c || exit 2
rm -rf /tmp/ext2_posix_host; mkdir -p /tmp/ext2_posix_host; sysroot/usr/bin/ext2_posix /tmp/ext2_posix_host/pc > $W/posix_host.txt
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
trap 'rm -f sysroot/usr/bin/ext2_posix' EXIT
X=/usr/bin/ext2_posix
bash scripts/qemu-boot-test.sh -t "${T:-900}" \
 "sleep 45" \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?' \
 "echo @@EXT2; $X /data/pc; echo @@END" \
 'printf "#!/bin/sh\necho EXEC_OK\n" > /data/x.sh; chmod +x /data/x.sh; echo @@NX0; /data/x.sh; echo rc=$?' \
 "$X --remount /data 8; echo @@NX1; /data/x.sh; echo rc=\$?; $X --remount /data 0; echo @@NX2; /data/x.sh; echo rc=\$?" \
 "$X --remount /data 4; echo @@ND1; $X --open /data/pc/cd; $X --remount /data 0; echo @@ND2; $X --open /data/pc/cd" \
 "$X --remount /data 16; echo @@SYNCMNT; dd if=/dev/zero of=/data/syncfile bs=4096 count=64 2>&1 | tail -1; $X --remount /data 0" \
 'umount /data; echo @@UM rc=$?' 'echo @@DONE' 'power off' >/dev/null 2>&1
sec() { tr -d '\r' < $LOG | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -v -E '^\[ *[0-9.]+\]'; }
sec EXT2 > $W/$TAG.ext2.txt
echo "== casos: guest=$(wc -l < $W/$TAG.ext2.txt) linux=$(wc -l < $W/posix_host.txt)"
diff $W/posix_host.txt $W/$TAG.ext2.txt && echo "ext2 == Linux en todos los casos"
echo "--- flags de montaje:"
tr -d '\r' < $LOG | grep -a -v -E '^\[ *[0-9.]+\]' | grep -a -E '^(@@(NX|ND|SYNCMNT|UM|DONE|MOUNT)|EXEC_OK|rc=|remount |open |.*denied|.*[0-9]+ bytes|.*Permission)' | tr '\n' ' '; echo
e2fsck -fn $IMG > $W/$TAG.fsck 2>&1; rc=$?; echo "e2fsck -fn rc=$rc"; [ $rc -ne 0 ] && head -20 $W/$TAG.fsck
exit $rc
