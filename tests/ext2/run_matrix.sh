#!/bin/bash
# Pasa run_read_test.sh por toda la matriz de imágenes y comprueba que ext4 se
# rechaza con un mensaje. Resumen al final; código != 0 si algo falla.
cd "$(dirname "$0")/../.."
DIR=${1:-build/ext2-test}
fail=0
for n in r1_1kdef r0_1k r1_1k r1_2k r1_4k r1_4k128 r1_g2048 ext3; do
  echo "################ $n"
  T=${T:-420} tests/ext2/run_read_test.sh "$DIR/$n.img" "$DIR" || fail=1
done
echo "################ ext4 (debe rechazarse)"
export WITH_NVIDIA=${WITH_NVIDIA:-0}
export QEMU_EXTRA="-drive file=$DIR/ext4.img,if=none,id=e2,format=raw,snapshot=on -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
LOG=/tmp/ext2-read-ext4.log bash scripts/qemu-boot-test.sh -t 150 "sleep 40" "power off" >/dev/null 2>&1
if grep -a -q "features incompatibles no soportadas" /tmp/ext2-read-ext4.log && ! grep -a -q "EXT2\] montado" /tmp/ext2-read-ext4.log; then
  echo "ext4: rechazado correctamente: $(grep -a 'features incompatibles' /tmp/ext2-read-ext4.log | head -1 | cut -c1-200)"
else echo "ext4: FALLO (no se rechazó con el mensaje esperado)"; fail=1; fi
echo "RESULTADO MATRIZ: $([ $fail = 0 ] && echo OK || echo FALLOS)"
exit $fail
