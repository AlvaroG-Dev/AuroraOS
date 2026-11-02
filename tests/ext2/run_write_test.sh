#!/bin/bash
# E2: escribe en un ext2 desde AuroraOS (QEMU) y verifica en el host con
# e2fsck -fn (oraculo) + rdump de debugfs (contenido).
#   BS=1024|4096 ISZ=128|256 SIZE=MB NFILES=n CLEAN=umount|poweroff tests/ext2/run_write_test.sh
set -u
cd "$(dirname "$0")/../.."
BS=${BS:-1024}; ISZ=${ISZ:-128}; SIZE=${SIZE:-512}; NFILES=${NFILES:-2000}
CLEAN=${CLEAN:-umount}; TAG=${TAG:-w_${BS}_${ISZ}_$CLEAN}
W=build/ext2-write; mkdir -p $W
IMG=$W/$TAG.img; LOG=${LOG:-/tmp/ext2-write-$TAG.log}
rm -f $IMG
mke2fs -q -F -t ext2 -b $BS -I $ISZ $IMG ${SIZE}M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
END='umount /data; echo @@UMOUNT_RC=$?'
[ "$CLEAN" = poweroff ] && END='echo @@NOUMOUNT'
bash scripts/qemu-boot-test.sh -t "${T:-900}" \
 "sleep ${SETTLE:-45}" \
 'umount /data; echo @@UMOUNT0 rc=$?' \
 'mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?; grep -a " /data " /proc/self/mountinfo' \
 "cd /data && mkdir -p t/a/b/c/d t/many t/dir2 && cd t/many && for i in \$(seq 1 $NFILES); do : > f\$i; done; echo @@MADE; ls -f | wc -l" \
 'cd /data/t && for i in 1 2 3 5 8 13 21 34; do seq 1 $((i*700)) > data$i; done; seq 1 1000000 > seqbig; echo @@WROTE' \
 'cd /data/t && dd if=/dev/null of=sparse bs=1 seek=314572800 2>/dev/null && printf FIN | dd of=sparse bs=1 seek=314572797 conv=notrunc 2>/dev/null && printf INI | dd of=sparse bs=1 seek=0 conv=notrunc 2>/dev/null; echo @@SPARSE rc=$?' \
 'cd /data/t && seq 1 5000 > shrink && dd if=/dev/null of=shrink bs=1 seek=1000 2>/dev/null && cp shrink shrink_copy && seq 1 3000 > grow && dd if=/dev/null of=grow bs=1 seek=90000 2>/dev/null; echo @@TRUNC rc=$?' \
 'cd /data/t && mkfifo ff && ln -s data1 sym_short && ln -s /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa sym_long && chmod 640 data2 && touch -d @1700000000 data3 && echo @@MISC rc=$?' \
 'cd /data/t && echo hola > a/b/c/d/hoja && cp data5 dir2/copy5 && rm data8 && rm -rf dir2 && mkdir dir3 && rmdir dir3 && rm many/f1 many/f2 && echo @@RM rc=$?' \
 'cd /data/t && echo @@SHA && find . -type f ! -path "./many/*" ! -name sparse -exec sha256sum {} \; ; echo @@END' \
 'cd /data/t && echo @@LS && find . ! -path "./many/*" -printf "%p %y %m %s\n"; ls -f many | wc -l; head -c 3 sparse; tail -c 3 sparse; echo; echo @@END' \
 'sync; echo @@SYNCED' \
 "$END" \
 'echo @@DONE' 'power off' >/dev/null 2>&1
sec() { tr -d '\r' < "$LOG" | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -v -E '^\[ *[0-9.]+\]'; }
fail=0
echo "== $TAG"
grep -a -E '@@(UMOUNT0|MOUNT|MADE|WROTE|SPARSE|TRUNC|MISC|RM|SYNCED|UMOUNT_RC|NOUMOUNT)' "$LOG" | tr -d '\r' | cut -c1-120 | tr '\n' ' '; echo
grep -a -E '\[EXT2\]' "$LOG" | tr -d '\r' | cut -c1-160 | head -6
if ! grep -a -q '@@DONE' "$LOG"; then echo "FALLO: no termino (timeout)"; fail=1; fi
e2fsck -fn $IMG > $W/$TAG.fsck 2>&1; rc=$?
echo "e2fsck -fn rc=$rc"; [ $rc -ne 0 ] && { head -20 $W/$TAG.fsck; fail=1; }
rm -rf $W/$TAG.out; mkdir -p $W/$TAG.out
debugfs -R "rdump /t $W/$TAG.out" $IMG >/dev/null 2>&1
H=$W/$TAG.out/t
if [ -d "$H" ]; then
  (cd $H && find . -type f ! -path './many/*' ! -name sparse -exec sha256sum {} \; | LC_ALL=C sort -k2) > $W/$TAG.host.sha
  sec SHA | grep -a -E '^[0-9a-f]{64}  \./' | LC_ALL=C sort -k2 -u > $W/$TAG.guest.sha
  if diff $W/$TAG.host.sha $W/$TAG.guest.sha >/dev/null; then echo "sha256: OK ($(wc -l < $W/$TAG.host.sha) ficheros)"; else echo "sha256: DIFERENCIAS"; diff $W/$TAG.host.sha $W/$TAG.guest.sha | head; fail=1; fi
  echo "many en host: $(ls -f $H/many | wc -l) (esperado $((NFILES-2+2)) con . y ..)"
  echo "sparse en host: $(stat -c %s $H/sparse) bytes, bloques=$(stat -c %b $H/sparse) ini=$(head -c 3 $H/sparse) fin=$(tail -c 3 $H/sparse)"
else echo "FALLO: rdump no extrajo /t"; fail=1; fi
echo "-- guest ls (muestra):"; sec LS | tail -4
exit $fail
