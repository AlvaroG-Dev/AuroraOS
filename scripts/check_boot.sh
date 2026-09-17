#!/usr/bin/env bash
# scripts/check_boot.sh
#
# Arranca Aurora OS en QEMU headless, captura el serial, y verifica que
# las cadenas esperadas aparecen. Falla con exit code != 0 si algo no
# está.
#
# Uso:
#   ./scripts/check_boot.sh [timeout_seconds]
#
# Requiere:
#   - aurora.iso en la raíz (o aurora.img). El script regenera si hace falta.
#   - qemu-system-x86_64
#   - OVMF (ruta /usr/share/OVMF/OVMF_CODE_4M.fd por defecto)

set -euo pipefail

TIMEOUT="${1:-30}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERIAL_LOG="$ROOT/serial.log"
QEMU_LOG="$ROOT/qemu.log"

OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_SRC="${OVMF_VARS_SRC:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
OVMF_VARS="$ROOT/OVMF_VARS.fd"

cd "$ROOT"

if [ ! -f "$ROOT/aurora.iso" ]; then
    echo "[check_boot] aurora.iso no existe, compilando..."
    make iso
fi

if [ ! -f "$OVMF_CODE" ]; then
    echo "[check_boot] ERROR: no encuentro OVMF en $OVMF_CODE" >&2
    echo "[check_boot] Instala el paquete ovmf o exporta OVMF_CODE=/ruta/a/OVMF_CODE_4M.fd" >&2
    exit 1
fi

cp "$OVMF_VARS_SRC" "$OVMF_VARS"

rm -f "$SERIAL_LOG" "$QEMU_LOG"

echo "[check_boot] Arrancando QEMU con timeout de ${TIMEOUT}s..."

# -no-reboot y -no-shutdown hacen que QEMU no se cierre solo al triple fault.
# Usamos 'timeout' del coreutils para matarlo tras TIMEOUT segundos.
set +e
timeout --foreground "$TIMEOUT" qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -cdrom "$ROOT/aurora.iso" \
    -serial file:"$SERIAL_LOG" \
    -display none \
    -m 512M \
    -cpu qemu64 \
    -no-reboot -no-shutdown \
    > "$QEMU_LOG" 2>&1
QEMU_EXIT=$?
set -e

# QEMU matado por timeout es OK (el kernel se queda en idle).
if [ "$QEMU_EXIT" != "0" ] && [ "$QEMU_EXIT" != "124" ]; then
    echo "[check_boot] ERROR: QEMU salió con código $QEMU_EXIT"
    echo "--- qemu.log ---"
    cat "$QEMU_LOG"
    exit 1
fi

if [ ! -s "$SERIAL_LOG" ]; then
    echo "[check_boot] ERROR: serial.log está vacío"
    echo "--- qemu.log ---"
    cat "$QEMU_LOG"
    exit 1
fi

echo "[check_boot] Verificando cadenas esperadas..."

EXPECTED=(
    "AURORA OS KERNEL x86_64"
    "[PMM] Bitmap inicializado"
    "[PAGING] Ventana fisica mapeada"
    "[HEAP] Heap inicializado"
    "[SLAB] Inicializado: 8 caches"
    "[TARFS] Carga completa. 21 nodos registrados."
    "[SCHED] Scheduler + SSE/FPU inicializado"
    "[IPC-KERNEL] Servicio de eco IPC iniciado"
    "[TTY] /dev/tty0 inicializado"
    "[PS2] Tarea de procesamiento iniciada"
    "[COMP] Compositor thread started"
    "18 tests: 18 passed, 0 failed, 0 skipped"
    "Aurora OS Shell v0.1"
    "aurora>"
)

FAILED=0
for needle in "${EXPECTED[@]}"; do
    if grep -qF "$needle" "$SERIAL_LOG"; then
        echo "  [OK]   $needle"
    else
        echo "  [FAIL] $needle"
        FAILED=$((FAILED + 1))
    fi
done

if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo "[check_boot] $FAILED cadenas no encontradas."
    echo "--- serial.log (últimas 200 líneas) ---"
    tail -n 200 "$SERIAL_LOG"
    exit 1
fi

echo ""
echo "[check_boot] Todas las cadenas esperadas presentes. OK."
exit 0