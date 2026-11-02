#!/bin/bash
# E6: e2fsck del propio AuroraOS repara un ext2 estropeado en el host
# (bitmap con un bloque en uso marcado libre + inodo libre con datos + contadores
# falsos + FS sin desmontar) y el host comprueba que queda limpio.
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-e6; mkdir -p $W/seed; IMG=$W/repair.img; LOG=${LOG:-/tmp/ext2-repair.log}
seq 1 20000 > $W/seed/a; seq 1 300 > $W/seed/b; mkdir -p $W/seed/d; seq 1 50 > $W/seed/d/c
rm -f $IMG; mke2fs -q -F -t ext2 -b 1024 -d $W/seed $IMG 8M || exit 2
B=$(debugfs -R 'bmap /a 3' $IMG 2>/dev/null)
debugfs -w -R "freeb $B" $IMG >/dev/null 2>&1
debugfs -w -R 'set_bg 0 free_blocks_count 7' $IMG >/dev/null 2>&1
debugfs -w -R 'ssv state 0' $IMG >/dev/null 2>&1
echo "roto: e2fsck -fn en host:"; e2fsck -fn $IMG 2>&1 | grep -c "Fix"
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "${T:-300}" 'sleep 40' \
 'e2fsck -fy /dev/sda; echo @@FIX rc=$?' \
 'e2fsck -fy /dev/sda; echo @@FIX2 rc=$?' \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a -E "^@@|^Pass|Block bitmap|differ|FILE SYSTEM|files \(|Free blocks" | grep -av "INFO\|DEBUG\|WARN" | cut -c1-120
e2fsck -fn $IMG >$W/repair.fsck 2>&1; rc=$?; echo "host e2fsck -fn rc=$rc"
cmp <(debugfs -R 'cat /a' $IMG 2>/dev/null) $W/seed/a && echo "a: IGUAL"
exit $rc
