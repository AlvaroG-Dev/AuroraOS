#!/bin/bash
# chattr/lsattr (inmutable, append-only), nosuid y mknod FIFO/socket sobre ext2.
set -u
cd "$(dirname "$0")/../.."
W=build/ext2-pending; IMG=$W/p.img; LOG=/tmp/ext2-pending.log; mkdir -p $W
rm -f $IMG; mke2fs -q -F -t ext2 -b 4096 -L AURORADATA $IMG 64M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG QEMU_KILL_SIG=KILL
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
cat > sysroot/system/runas.py <<'PYEOF'
import os
pid = os.fork()
if pid == 0:
    os.setgid(1000); os.setuid(1000)
    os.execv(os.environ.get("IDSU", "/data/idsu"), ["idsu", "-u"])
_, st = os.waitpid(pid, 0)
PYEOF
trap 'rm -f sysroot/system/runas.py' EXIT
RUNAS="python3 /system/runas.py"
bash scripts/qemu-boot-test.sh -t "${T:-600}" 'sleep 40' \
 'cd /data; echo uno > f; chattr +i f; echo "@@LS $(lsattr f)"; (echo dos >> f) 2>&1 | sed "s/^/@@IAPP /"; rm f 2>&1 | sed "s/^/@@IRM /"; mv f g 2>&1 | sed "s/^/@@IMV /"; chattr -i f; rm f && echo @@IRM_OK' \
 'cd /data; echo uno > a; chattr +a a; echo "@@LSA $(lsattr a)"; echo dos >> a && echo @@APP_OK; (echo tres > a) 2>&1 | sed "s/^/@@AOVR /"' \
 'cd /data; (truncate -s 0 a) 2>&1 | sed "s/^/@@ATRU /"; rm a 2>&1 | sed "s/^/@@ARM /"; echo "@@ACAT $(cat a | tr "\n" " ")"; chattr -a a; rm a && echo @@ARM_OK' \
 'cd /data; python3 -c "import os,stat; os.mkfifo(\"ff\"); print(\"@@FIFO\", stat.S_ISFIFO(os.stat(\"ff\").st_mode))"; ls -l ff | sed "s/^/@@LSF /"' \
 "cd /data; cp /usr/bin/id idplain; chmod 755 idplain; echo @@PLAIN \$(IDSU=/data/idplain $RUNAS 2>&1)" \
 "cp /usr/bin/id /tmp/idt; chown 0:0 /tmp/idt; chmod 4755 /tmp/idt; echo @@TMPSUID \$(IDSU=/tmp/idt $RUNAS 2>&1)" \
  "cd /data; cp /usr/bin/id idsu; chmod 4755 idsu; echo @@SUID_RW \$($RUNAS 2>&1)" \
 "cd /data; mount -o remount,nosuid /data; echo @@REM rc=\$?; echo @@SUID_NOSUID \$($RUNAS 2>&1)" \
 'echo @@DONE; power off' >/dev/null 2>&1
tr -d '\r' < $LOG | grep -a "@@" | grep -av "bash -c" | cut -c1-160
