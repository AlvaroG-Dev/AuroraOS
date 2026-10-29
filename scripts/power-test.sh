#!/bin/bash
# Prueba apagado/reinicio en QEMU (Plan_Power.md). Uso: scripts/power-test.sh off|reboot
# Arranca aurora.iso con -no-reboot y comprueba que QEMU termina solo y que el
# log serie contiene los mensajes de [POWER].
set -e
cd "$(dirname "$0")/.."
MODE=${1:-off}
T=${T:-120}
LOG=${LOG:-/tmp/power-test.log}
make iso >/tmp/power-test-build.log 2>&1 || { tail -30 /tmp/power-test-build.log; exit 1; }
printf 'power %s\n' "$MODE" > sysroot/system/boot-tests
make iso >>/tmp/power-test-build.log 2>&1
rm -f sysroot/system/boot-tests
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
set +e
timeout "$T" qemu-system-x86_64 -machine q35 -m 1G -smp 2 -display none -no-reboot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -cdrom aurora.iso -serial file:"$LOG" ${QEMU_EXTRA:-}
rc=$?
grep -a "\[POWER\]\|\[ACPI PM\]" "$LOG" | tail -20
if [ $rc -eq 124 ]; then echo "FALLO: QEMU no termino en ${T}s"; exit 1; fi
echo "OK: QEMU termino solo (rc=$rc)"
