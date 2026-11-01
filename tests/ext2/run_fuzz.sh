#!/bin/bash
# Fuzz de imágenes corruptas: arranca AuroraOS con hasta 6 imágenes mutadas
# como discos AHCI y para cada una intenta montar + recorrer todo con tar.
# Criterio: el kernel NO debe caerse (panic, cuelgue) ni quedarse en bucle.
# El montaje puede fallar y las lecturas dar error: eso es lo correcto.
#   tests/ext2/run_fuzz.sh [semilla] [n<=6]
set -u
cd "$(dirname "$0")/../.."
SEED=${1:-1}; N=${2:-6}; DIR=build/ext2-fuzz
python3 tests/ext2/gen_fuzz.py "$DIR" "$SEED" "$N" >/dev/null || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0}
LOG=${LOG:-/tmp/ext2-fuzz-$SEED.log}; export LOG
args=""; cmds=()
for ((i=0;i<N;i++)); do
  args+=" -drive file=$PWD/$DIR/fuzz$i.img,if=none,id=f$i,format=raw,snapshot=on -device ide-hd,drive=f$i,bus=ahci0.$i"
  dev=sd$(printf "\\x$(printf %x $((97+i)))")
  cmds+=("mkdir -p /tmp/m$i; echo @@F$i-mount; mount -t ext2 /dev/$dev /tmp/m$i; echo @@F$i-mount-rc=\$?; timeout 90 tar cf - /tmp/m$i 2>&1 | wc -c; du -s /tmp/m$i 2>&1 | tail -1; umount /tmp/m$i; echo @@F$i-end")
done
export QEMU_EXTRA="-device ahci,id=ahci0 $args"
bash scripts/qemu-boot-test.sh -t "${T:-420}" "sleep ${SETTLE:-60}" "${cmds[@]}" "echo @@FDONE" "power off" >/dev/null 2>&1
rc=0
tr -d '\r' < "$LOG" > "$LOG.clean"
for ((i=0;i<N;i++)); do
  echo "fuzz$i: $(grep -a -E "^@@F$i-mount-rc=" "$LOG.clean" | head -1) $(grep -a -E "EXT2\] sd$(printf "\\x$(printf %x $((97+i)))")" "$LOG.clean" | head -1 | sed 's/^\[[ 0-9.]*\] //' | cut -c1-110)"
done
if grep -a -q -E 'PANIC|System Halted|KERNEL PANIC' "$LOG.clean"; then echo "FALLO: panic del kernel"; grep -a -m3 -E 'PANIC ==>|panic:' "$LOG.clean" | cut -c1-200; rc=1; fi
if ! grep -a -q '^@@FDONE' "$LOG.clean"; then echo "FALLO: no llegó al final (cuelgue/timeout)"; rc=1; fi
echo "RESULTADO FUZZ seed=$SEED: $([ $rc = 0 ] && echo OK || echo FALLO)"
exit $rc
