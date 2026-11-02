#!/bin/bash
# Prueba el boton de encendido ACPI en QEMU: arranca la ISO, espera a que el
# sondeo este activo y envia system_powerdown por el monitor. Debe aparecer
# "[POWER] apagado solicitado (boton de encendido)" y QEMU terminar solo.
# Variables: T (timeout), MACHINE (q35 por defecto), LOG.
set -e
cd "$(dirname "$0")/.."
T=${T:-180}
LOG=${LOG:-/tmp/power-button.log}
SOCK=/tmp/power-button.mon
rm -f "$LOG" "$SOCK"
make iso >/tmp/power-button-build.log 2>&1 || { tail -30 /tmp/power-button-build.log; exit 1; }
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
timeout "$T" qemu-system-x86_64 -machine "${MACHINE:-q35}" -m 1G -smp 2 -display none -no-reboot \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -cdrom aurora.iso -serial file:"$LOG" -monitor unix:"$SOCK",server,nowait &
QPID=$!
for _ in $(seq 1 90); do
  grep -aq "boton de encendido fijo" "$LOG" 2>/dev/null && break
  sleep 2
done
sleep 15   # que el sistema termine de arrancar
echo system_powerdown | nc -U "$SOCK" -q1 >/dev/null || true
set +e
wait $QPID
rc=$?
grep -a "\[POWER\]\|boton de encendido" "$LOG" | tail -6
if [ $rc -eq 124 ]; then echo "FALLO: QEMU no termino"; exit 1; fi
grep -aq "apagado solicitado (boton de encendido)" "$LOG" || { echo "FALLO: sin evento de boton"; exit 1; }
echo "OK: boton de encendido apaga el sistema (rc=$rc)"
