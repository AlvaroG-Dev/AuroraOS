#!/usr/bin/env bash
# scripts/check_boot.sh
#
# Arranca Aurora OS en QEMU headless, captura el serial y verifica que
# las cadenas esperadas aparecen. Falla con exit code != 0 si algo no
# está presente.
#
# Uso:
#   ./scripts/check_boot.sh [timeout_seconds]
#
# Variables de entorno:
#   TIMEOUT       (default: 60)      segundos antes de matar QEMU
#   OVMF_CODE     (default: /usr/share/OVMF/OVMF_CODE_4M.fd)
#   OVMF_VARS_SRC (default: /usr/share/OVMF/OVMF_VARS_4M.fd)
#   QEMU_BIN      (default: qemu-system-x86_64)
#   QEMU_CPU      (default: max)
#   QEMU_SMP      (default: 1)       en CI sin KVM, 1. En local con KVM, 8.
#   QEMU_MEM      (default: 512M)
#   USE_KVM       (default: auto)    auto|yes|no
#   EXPECT_TESTS  (default: 36)      número total de tests registrados
#   EXPECT_FAILED (default: 0)       tests que deben fallar
#   KEEP_LOGS     (default: no)      yes para no borrar serial.log/qemu.log
#
# Requiere:
#   - aurora.iso en la raíz (o se genera con `make iso`).
#   - qemu-system-x86_64
#   - OVMF (ruta /usr/share/OVMF/OVMF_CODE_4M.fd por defecto)

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuración
# ---------------------------------------------------------------------------
TIMEOUT="${TIMEOUT:-60}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERIAL_LOG="$ROOT/serial.log"
QEMU_LOG="$ROOT/qemu.log"

OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_SRC="${OVMF_VARS_SRC:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
OVMF_VARS="$ROOT/OVMF_VARS.fd"

QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
QEMU_CPU="${QEMU_CPU:-max}"
QEMU_SMP="${QEMU_SMP:-1}"
QEMU_MEM="${QEMU_MEM:-512M}"
USE_KVM="${USE_KVM:-auto}"
EXPECT_TESTS="${EXPECT_TESTS:-36}"
EXPECT_FAILED="${EXPECT_FAILED:-0}"
KEEP_LOGS="${KEEP_LOGS:-no}"

cd "$ROOT"

# ---------------------------------------------------------------------------
# Colores (solo si la salida es un TTY)
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    C_RED=$'\033[0;31m'
    C_GRN=$'\033[0;32m'
    C_YLW=$'\033[0;33m'
    C_RST=$'\033[0m'
else
    C_RED=""; C_GRN=""; C_YLW=""; C_RST=""
fi

log_info()  { echo "[check_boot] $*"; }
log_ok()    { echo "  ${C_GRN}[OK]${C_RST}   $*"; }
log_fail()  { echo "  ${C_RED}[FAIL]${C_RST} $*"; }
log_warn()  { echo "  ${C_YLW}[WARN]${C_RST} $*"; }
log_error() { echo "${C_RED}[check_boot] ERROR:${C_RST} $*" >&2; }

# ---------------------------------------------------------------------------
# Detectar KVM
# ---------------------------------------------------------------------------
kvm_args=()
kvm_enabled=0
case "$USE_KVM" in
    yes)
        kvm_args=(-enable-kvm -cpu host)
        kvm_enabled=1
        ;;
    no)
        kvm_args=(-cpu "$QEMU_CPU")
        ;;
    auto|*)
        if [ -w /dev/kvm ] && [ -r /dev/kvm ]; then
            kvm_args=(-enable-kvm -cpu host)
            kvm_enabled=1
        else
            kvm_args=(-cpu "$QEMU_CPU")
        fi
        ;;
esac

if [ "$kvm_enabled" = "1" ]; then
    log_info "KVM habilitado (-enable-kvm -cpu host)"
else
    log_warn "KVM no disponible, usando TCG con -cpu $QEMU_CPU (más lento, APs no arrancan)"
fi

# ---------------------------------------------------------------------------
# Verificar dependencias
# ---------------------------------------------------------------------------
if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    log_error "no encuentro $QEMU_BIN en \$PATH"
    exit 1
fi

if [ ! -f "$OVMF_CODE" ]; then
    log_error "no encuentro OVMF en $OVMF_CODE"
    echo "  Instala el paquete ovmf o exporta OVMF_CODE=/ruta/a/OVMF_CODE_4M.fd" >&2
    exit 1
fi

if [ ! -f "$OVMF_VARS_SRC" ]; then
    log_error "no encuentro OVMF_VARS en $OVMF_VARS_SRC"
    echo "  Exporta OVMF_VARS_SRC=/ruta/a/OVMF_VARS_4M.fd" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Generar la ISO si no existe
# ---------------------------------------------------------------------------
if [ ! -f "$ROOT/aurora.iso" ]; then
    log_info "aurora.iso no existe, generando con 'make iso'..."
    if ! make iso; then
        log_error "'make iso' falló"
        exit 1
    fi
fi

# Copiar OVMF_VARS para que QEMU pueda escribir sin corromper el original.
cp "$OVMF_VARS_SRC" "$OVMF_VARS"

# ---------------------------------------------------------------------------
# Limpiar logs previos
# ---------------------------------------------------------------------------
if [ "$KEEP_LOGS" != "yes" ]; then
    rm -f "$SERIAL_LOG" "$QEMU_LOG"
fi

# ---------------------------------------------------------------------------
# Lanzar QEMU
# ---------------------------------------------------------------------------
log_info "Arrancando QEMU (timeout=${TIMEOUT}s, smp=${QEMU_SMP}, mem=${QEMU_MEM})..."

set +e
timeout --foreground "$TIMEOUT" "$QEMU_BIN" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    "${kvm_args[@]}" \
    -smp "$QEMU_SMP" \
    -m "$QEMU_MEM" \
    -cdrom "$ROOT/aurora.iso" \
    -serial file:"$SERIAL_LOG" \
    -display none \
    -no-reboot -no-shutdown \
    > "$QEMU_LOG" 2>&1
QEMU_EXIT=$?
set -e

# 124 = timeout(1) mató el proceso. Es OK (el kernel se queda en idle).
if [ "$QEMU_EXIT" != "0" ] && [ "$QEMU_EXIT" != "124" ]; then
    log_error "QEMU salió con código $QEMU_EXIT"
    echo "--- qemu.log ---" >&2
    cat "$QEMU_LOG" >&2
    exit 1
fi

if [ ! -s "$SERIAL_LOG" ]; then
    log_error "serial.log está vacío"
    echo "--- qemu.log ---" >&2
    cat "$QEMU_LOG" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Cadenas esperadas
# ---------------------------------------------------------------------------
log_info "Verificando cadenas esperadas..."

EXPECTED_BOOT=(
    "AURORA OS KERNEL x86_64"
    "[SSE] OSFXSR + OSXMMEXCPT habilitados"
    "[PMM] Bitmap inicializado"
    "[PAGING] Ventana fisica mapeada"
    "[PAGING] SMEP habilitado"
    "[PAGING] SMAP habilitado"
    "[ACPI] Parseo completo"
    "[APIC] LAPIC activo y configurado"
    "[PF] Demand paging inicializado"
    "[HEAP] Heap inicializado"
    "[SLAB] Inicializado: 8 caches"
    "[TARFS] Carga completa. 21 nodos registrados."
    "[RTC] CMOS RTC detectado"
    "[SCHED] Scheduler + SSE/FPU SMP inicializado"
    "[IPC] Subsistema de Paso de Mensajes inicializado."
    "[SYS] Servicio 'echo' registrado"
    "[IPC-KERNEL] Servicio de eco IPC iniciado"
    "[LAPIC-TIMER] Modo periódico"
    "[TTY] Modo raw inicializado"
    "[INPUT] Subsistema de input inicializado"
    "[PS2] Tarea de procesamiento iniciada"
    "[PCI] Enumeracion completada."
    "[FB] Inicializado en memoria"
    "[COMP] Compositor thread started"
    "[WINSRV] Inicializado"
    "[PROC] Proceso 'apps/shell' creado"
)

EXPECTED_TESTS=(
    "[TEST] ${EXPECT_TESTS} tests:"
    "[TEST] ${EXPECT_FAILED} failed"
)

EXPECTED_SHELL=(
    "SHELL: main begin"
    "SHELL: after win_create"
)

FAILED=0

check_block() {
    local block_name="$1"
    shift
    local needles=("$@")
    log_info "  Bloque: $block_name"
    for needle in "${needles[@]}"; do
        if grep -qF -- "$needle" "$SERIAL_LOG"; then
            log_ok "$needle"
        else
            log_fail "$needle"
            FAILED=$((FAILED + 1))
        fi
    done
}

check_block "arranque" "${EXPECTED_BOOT[@]}"
check_block "tests"    "${EXPECTED_TESTS[@]}"
check_block "shell"    "${EXPECTED_SHELL[@]}"

# ---------------------------------------------------------------------------
# Reporte
# ---------------------------------------------------------------------------
if [ "$FAILED" -gt 0 ]; then
    echo ""
    log_error "$FAILED cadenas no encontradas"
    echo "--- serial.log (últimas 200 líneas) ---" >&2
    tail -n 200 "$SERIAL_LOG" >&2
    exit 1
fi

log_info "Todas las cadenas esperadas presentes. ${C_GRN}OK${C_RST}."
exit 0