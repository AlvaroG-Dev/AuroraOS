#!/bin/bash
# Hito E6: desde AuroraOS, mke2fs sobre un disco en blanco, montarlo, copiar,
# umount, e2fsck -f (rc 0) y el host (e2fsck -fn, debugfs) lo da por bueno.
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-e6; mkdir -p $W; IMG=$W/e6.img; LOG=${LOG:-/tmp/ext2-e6.log}
rm -f $IMG; truncate -s ${SIZE:-64}M $IMG
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-420}" 'sleep 40' \
 'mke2fs -F -t ext2 -L E6TEST /dev/sda 2>&1 | tail -4; echo @@MKFS' \
 'mkdir -p /tmp/m; mount -t ext2 /dev/sda /tmp/m; echo @@MNT rc=$?' \
 'cp /usr/bin/bash /tmp/m/; seq 1 50000 > /tmp/m/seq; mkdir /tmp/m/d; echo hola > /tmp/m/d/f; ls /tmp/m; echo @@CP' \
 'umount /tmp/m; echo @@UM rc=$?' \
 'e2fsck -fp /dev/sda; echo @@FSCK rc=$?' \
 'tune2fs -l /dev/sda | head -4; dumpe2fs -h /dev/sda 2>&1 | grep -i "features\|state"; echo @@T' \
 'echo @@DONE; power off' >/tmp/e6-run.out 2>&1
tr -d '\r' < $LOG | grep -a -E "^@@|E6TEST|files|Filesystem|features|state|Creating|Writing|^bash|^seq|^d$|lost" | grep -av "DEBUG\|INFO\|WARN" | cut -c1-140
fail=0
grep -a -q '@@DONE' $LOG || { echo "FALLO: no terminó"; fail=1; }
e2fsck -fn $IMG >$W/host.fsck 2>&1; rc=$?; echo "host e2fsck -fn rc=$rc"; [ $rc -ne 0 ] && { head $W/host.fsck; fail=1; }
dumpe2fs -h $IMG 2>/dev/null | grep -E "volume name|features|Block size"
debugfs -R 'cat /d/f' $IMG 2>/dev/null; debugfs -R 'cat /seq' $IMG 2>/dev/null | sha256sum | cut -c1-16; seq 1 50000 | sha256sum | cut -c1-16
cmp <(debugfs -R 'cat /bash' $IMG 2>/dev/null) sysroot/usr/bin/bash && echo "bash copiado: IGUAL" || { echo "bash: DIFERENTE"; fail=1; }
exit $fail
