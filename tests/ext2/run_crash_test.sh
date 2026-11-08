#!/bin/bash
# E4: cortes de energia simulados. Cada iteracion arranca AuroraOS con un ext2
# que tiene un arbol "stable" que nadie toca, monta rw, ejecuta una carga
# aleatoria (crear/borrar/renombrar/truncar/escribir) y mata QEMU con SIGKILL en
# un instante aleatorio. Luego en el host: e2fsck -fy (1 pasada) + e2fsck -fn
# (debe dar 0), sin "Multiply-claimed"/duplicados, y los ficheros stable intactos.
#   N=iteraciones SEED=n tests/ext2/run_crash_test.sh
set -u
cd "$(dirname "$0")/../.."
N=${N:-10}; SEED=${SEED:-1}; BS=${BS:-1024}
W=${WDIR:-build/ext2-crash}; mkdir -p $W
BASE=$W/base.img
if [ ! -f $BASE ] || [ -n "${REBASE:-}" ]; then
  rm -rf $W/tree; mkdir -p $W/tree/stable
  for d in 0 1 2 3 4 5; do mkdir -p $W/tree/stable/s$d; for f in 1 2 3 4 5 6 7 8; do seq 1 $((d*900+f*300)) > $W/tree/stable/s$d/file$f; done; done
  head -c 3000000 /dev/urandom > $W/tree/stable/s0/blob
  [ -f $W/src.tar ] && cp $W/src.tar $W/tree/stable/
  mkdir -p $W/tree/w
  rm -f $BASE; mke2fs -q -F -t ${FSTYPE:-ext2} ${MKOPTS:-} -b $BS -I 256 -d $W/tree $BASE ${SIZE:-128M} || exit 2
  [ -n "${JCSUM:-}" ] && printf 'jo -c\njc\n' | debugfs -w -f - $BASE >/dev/null 2>&1
  (cd $W/tree/stable && find . -type f -exec sha256sum {} \; | LC_ALL=C sort -k2) > $W/stable.sha
fi
export WITH_NVIDIA=${WITH_NVIDIA:-0}
cp tests/ext2/${LOADFILE:-crash_load.sh} sysroot/system/crash_load.sh
trap 'rm -f sysroot/system/crash_load.sh' EXIT
LOAD='bash /system/crash_load.sh'
RANDOM=$SEED
fail=0; first=1
for it in $(seq 1 $N); do
  IMG=$W/it.img; cp $BASE $IMG
  export LOG=/tmp/ext2-crash.log
  export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
  [ $first = 1 ] || export SKIP_BUILD=1; first=0
  rm -f $LOG
  bash scripts/qemu-boot-test.sh -t 900 \
    "sleep 25" 'umount /data; mount -t ${FSTYPE:-ext2} /dev/sda /data; echo @@MOUNT rc=$?' "$LOAD" >/dev/null 2>&1 &
  BG=$!
  # esperar al montaje rw (la velocidad de TCG varia mucho) y cortar tras un retardo aleatorio
  until pgrep -f "qemu-system-x86_64.*$(basename $W)/it.img" >/dev/null; do sleep 2; done  # (la 1a vez hay 'make iso')
  for _ in $(seq 1 600); do tr -d '\r' < $LOG 2>/dev/null | grep -a -q '@@MOUNT rc=' && break; sleep 1; done
  KT=$((${KMIN:-5} + RANDOM % ${KSPAN:-60}))
  sleep $KT
  pkill -KILL -f "qemu-system-x86_64.*$(basename $W)/it.img"
  wait $BG 2>/dev/null
  mounted=$(tr -d '\r' < $LOG | grep -a -c '@@MOUNT rc=0')
  jr=-; if [ "${FSTYPE:-ext2}" != ext2 ]; then cp $IMG $W/jr.img; debugfs -w -R jr $W/jr.img >/dev/null 2>&1; e2fsck -fn $W/jr.img > $W/jr.txt 2>&1; jr=$?; fi
  e2fsck -fy $IMG > $W/fsck1.txt 2>&1; rc1=$?
  e2fsck -fn $IMG > $W/fsck2.txt 2>&1; rc2=$?
  dup=$(grep -a -c -i 'multiply-claimed\|duplicate' $W/fsck1.txt)
  rm -rf $W/out; mkdir -p $W/out; debugfs -R "rdump /stable $W/out" $IMG >/dev/null 2>&1
  (cd $W/out/stable 2>/dev/null && find . -type f -exec sha256sum {} \; | LC_ALL=C sort -k2) > $W/after.sha
  if diff -q $W/stable.sha $W/after.sha >/dev/null; then st=OK; else st=DIFIERE; fi
  verdict=OK
  [ "$mounted" = 1 ] || verdict="NO-MONTO"
  [ $rc2 -eq 0 ] || verdict="FSCK2=$rc2"
  [ $dup -eq 0 ] || verdict="$verdict DUPLICADOS=$dup"
  [ $st = OK ] || verdict="$verdict STABLE-$st"
  echo "it=$it kill=+${KT}s mount=$mounted jr_only_rc=$jr fsck1_rc=$rc1 fsck2_rc=$rc2 dup=$dup stable=$st -> $verdict"
  if [ "$verdict" != OK ]; then fail=1; cp $IMG $W/fail_$it.img; cp $LOG $W/fail_$it.log; cp $W/fsck1.txt $W/fail_$it.fsck1; fi
done
exit $fail
