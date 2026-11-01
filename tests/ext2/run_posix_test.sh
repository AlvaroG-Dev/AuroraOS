#!/bin/bash
# E3: rename/link/unlink-abierto/chmod/chown/git sobre ext2 en QEMU + e2fsck -fn.
#   BS=1024|4096 tests/ext2/run_posix_test.sh
set -u
cd "$(dirname "$0")/../.."
BS=${BS:-1024}; TAG=${TAG:-p_$BS}
W=build/ext2-write; mkdir -p $W; IMG=$W/$TAG.img; LOG=/tmp/ext2-posix-$TAG.log
rm -f $IMG; mke2fs -q -F -t ext2 -b $BS -I 256 $IMG 128M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-600}" \
 "sleep 45" \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?' \
 'cd /data && mkdir -p r/a r/b r/c/sub && echo uno > r/a/f1 && echo dos > r/a/f2 && echo tres > r/b/f3 && mv r/a/f1 r/a/f1b; echo @@R1 rc=$?; cat r/a/f1b' \
 'cd /data/r && mv a/f1b a/f2; echo @@R2 rc=$?; cat a/f2; ls a' \
 'cd /data/r && mv a/f2 b/f2m; echo @@R3 rc=$?; ls a b' \
 'cd /data/r && mv c d; echo @@R4 rc=$?; ls d; mv d/sub b/; echo @@R5 rc=$?; ls b d' \
 'cd /data/r && mkdir e e/x n n/full n/full/k && mv e/x n/full 2>&1; echo @@R6 rc=$? "(1 esperado: ENOTEMPTY)"; mv e e/x/zz 2>&1; echo @@R7 rc=$? "(1 esperado: dentro de si mismo)"; mv e/x n/vacio; echo @@R8 rc=$?; ls n; stat -c "nlink e=%h n=%h" e n' \
 'cd /data/r && echo hola > h1 && ln h1 h2 && ln -s h1 s1 && echo @@L1 rc=$?; ls -l h1 h2 | cut -c1-60; rm h1; cat h2; stat -c "nlink=%h" h2' \
 'cd /data/r && echo abierto > ab && exec 3<ab && rm ab && cat <&3; ls ab 2>&1 | head -1; exec 3<&-; echo @@UO' \
 'cd /data && echo sec > priv && chown 1000:100 priv && chmod 600 priv && stat -c "%u %g %a" priv; echo @@OWN' \
 'cd /data && mkdir -p g && cd g && git init -q . && echo x > a && git add a && git -c user.name=a -c user.email=a@a commit -qm uno && git log --oneline | wc -l; echo @@GIT rc=$?' \
 'umount /data; echo @@UM rc=$?' \
 'echo @@DONE' 'power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a -v -E '^\[ *[0-9.]+\]' | awk '/^@@MOUNT/,/^@@DONE/' | cut -c1-110
e2fsck -fn $IMG > $W/$TAG.fsck 2>&1; rc=$?; echo "e2fsck -fn rc=$rc"; [ $rc -ne 0 ] && head -20 $W/$TAG.fsck
debugfs -R 'stat /priv' $IMG 2>/dev/null | head -3 | tr '\n' ' '; echo
exit $rc
