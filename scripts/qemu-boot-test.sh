#!/bin/bash
# Arranca aurora.iso en QEMU sin pantalla con /system/boot-tests = "$@" (una orden
# por argumento) y deja la salida serie en $LOG (por defecto /tmp/boot-test.log).
#
# Uso: [QMON="sendkey a;mouse_move 20 10"] [QMON_AT=14] \
#        scripts/qemu-boot-test.sh [-t segundos] orden1 orden2 ...
#
# QMON: ordenes del monitor de QEMU (separadas por ';') que se envian cuando
# aparece QMON_WAIT en el log (o QMON_AT segundos despues de arrancar si no
# se da); sirve para inyectar teclado/raton.
set -e
cd "$(dirname "$0")/.."
T=90
if [ "$1" = "-t" ]; then T=$2; shift 2; fi
LOG=${LOG:-/tmp/boot-test.log}
rm -f "$LOG"   # sin restos de una ejecucion anterior (QMON_WAIT los confundiria)
printf '%s\n' "$@" > sysroot/system/boot-tests
trap 'rm -f sysroot/system/boot-tests' EXIT
make iso >/tmp/boot-test-build.log 2>&1 || { tail -30 /tmp/boot-test-build.log; exit 1; }
rm -f sysroot/system/boot-tests
trap - EXIT
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
rm -f "$LOG" ${QMON_SOCK:-/tmp/qmon.sock}

if [ -n "${QMON:-}" ]; then
  (
    if [ -n "${QMON_WAIT:-}" ]; then
      # esperar a que el test imprima el marcador (hasta ~120 s)
      for _ in $(seq 1 240); do
        grep -aq "$QMON_WAIT" "$LOG" 2>/dev/null && break
        sleep 0.5
      done
      sleep 1
    else
      sleep "${QMON_AT:-14}"
    fi
    python3 - "$QMON" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
for _ in range(20):
    try:
        s.connect(__import__('os').environ.get('QMON_SOCK', '/tmp/qmon.sock'))
        break
    except Exception:
        time.sleep(0.5)
for c in sys.argv[1].split(';'):
    s.send((c.strip() + '\n').encode())
    time.sleep(0.4)
time.sleep(1)
PY
  ) &
fi

timeout "$T" qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -m "${QEMU_MEM:-512M}" -no-reboot -smp "${QEMU_SMP:-2}" -cdrom aurora.iso -display none \
  -monitor unix:${QMON_SOCK:-/tmp/qmon.sock},server,nowait \
  -serial file:"$LOG" || true
