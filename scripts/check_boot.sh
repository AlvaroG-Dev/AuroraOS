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
#   TIMEOUT        (default: 90)     segundos antes de matar QEMU
#   OVMF_CODE      (default: /usr/share/OVMF/OVMF_CODE_4M.fd)
#   OVMF_VARS_SRC  (default: /usr/share/OVMF/OVMF_VARS_4M.fd)
#   QEMU_BIN       (default: qemu-system-x86_64)
#   QEMU_CPU       (default: max)
#   QEMU_SMP       (default: 1)      CPUs a emular. >1 requiere KVM para APs.
#   QEMU_MEM       (default: 512M)
#   USE_KVM        (default: auto)   auto|yes|no
#   KEEP_LOGS      (default: no)     yes para no borrar serial.log/qemu.log
#   REQUIRE_SMP    (default: auto)   auto: exige APs si QEMU_SMP>1 Y KVM.
#                                    yes:  siempre exige APs.
#                                    no:   nunca exige APs.
#   SHOW_SERIAL    (default: yes)    yes:  vuelca serial.log al final
#                                    no:   no vuelca nada
#                                    on-failure: solo si el script falla
#   SHOW_SERIAL_LINES (default: 0)   si >0, solo vuelca las últimas N líneas.
#                                    0 = todo el log.
#
# Requiere:
#   - aurora.iso en la raíz (o se genera con `make iso`).
#   - qemu-system-x86_64
#   - OVMF

set -euo pipefail

TIMEOUT="${TIMEOUT:-90}"
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
KEEP_LOGS="${KEEP_LOGS:-no}"
REQUIRE_SMP="${REQUIRE_SMP:-auto}"
SHOW_SERIAL="${SHOW_SERIAL:-yes}"
SHOW_SERIAL_LINES="${SHOW_SERIAL_LINES:-0}"

cd "$ROOT"

# ---------------------------------------------------------------------------
# Colores
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    C_RED=$'\033[0;31m'; C_GRN=$'\033[0;32m'
    C_YLW=$'\033[0;33m'; C_RST=$'\033[0m'
    C_CYN=$'\033[0;36m'
else
    C_RED=""; C_GRN=""; C_YLW=""; C_RST=""; C_CYN=""
fi

log_info()  { echo "[check_boot] $*"; }
log_ok()    { echo "  ${C_GRN}[OK]${C_RST}   $*"; }
log_fail()  { echo "  ${C_RED}[FAIL]${C_RST} $*"; }
log_warn()  { echo "  ${C_YLW}[WARN]${C_RST} $*"; }
log_error() { echo "${C_RED}[check_boot] ERROR:${C_RST} $*" >&2; }

# ---------------------------------------------------------------------------
# Volcado del serial.log
#
# Se llama al final del script, con o sin fallo, según SHOW_SERIAL.
# ---------------------------------------------------------------------------
dump_serial() {
    local force="$1"  # "force" para saltarse el filtro on-failure
    case "$SHOW_SERIAL" in
        no) return 0 ;;
        on-failure) [ "$force" = "force" ] || return 0 ;;
        yes|*) ;;
    esac

    if [ ! -f "$SERIAL_LOG" ]; then
        return 0
    fi

    echo ""
    echo "${C_CYN}==================== serial.log ====================${C_RST}"
    if [ "$SHOW_SERIAL_LINES" -gt 0 ]; then
        echo "${C_CYN}(últimas $SHOW_SERIAL_LINES líneas)${C_RST}"
        tail -n "$SHOW_SERIAL_LINES" "$SERIAL_LOG"
    else
        cat "$SERIAL_LOG"
    fi
    echo "${C_CYN}====================================================${C_RST}"
    echo ""
}

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
    log_info "KVM no disponible, usando TCG con -cpu $QEMU_CPU"
fi

# Resolver REQUIRE_SMP=auto
if [ "$REQUIRE_SMP" = "auto" ]; then
    if [ "$QEMU_SMP" -gt 1 ] && [ "$kvm_enabled" = "1" ]; then
        REQUIRE_SMP=yes
    else
        REQUIRE_SMP=no
    fi
fi

log_info "QEMU_SMP=$QEMU_SMP, KVM=$kvm_enabled, REQUIRE_SMP=$REQUIRE_SMP"

# ---------------------------------------------------------------------------
# Verificar dependencias
# ---------------------------------------------------------------------------
if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    log_error "no encuentro $QEMU_BIN en \$PATH"
    dump_serial force
    exit 1
fi

for f in "$OVMF_CODE" "$OVMF_VARS_SRC"; do
    if [ ! -f "$f" ]; then
        log_error "no encuentro OVMF: $f"
        dump_serial force
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Generar ISO si no existe
# ---------------------------------------------------------------------------
if [ ! -f "$ROOT/aurora.iso" ]; then
    log_info "aurora.iso no existe, generando con 'make iso'..."
    if ! make iso; then
        log_error "'make iso' falló"
        dump_serial force
        exit 1
    fi
fi

cp "$OVMF_VARS_SRC" "$OVMF_VARS"

if [ "$KEEP_LOGS" != "yes" ]; then
    rm -f "$SERIAL_LOG" "$QEMU_LOG"
fi

# ---------------------------------------------------------------------------
# Lanzar QEMU
# ---------------------------------------------------------------------------
log_info "Arrancando QEMU (timeout=${TIMEOUT}s, mem=${QEMU_MEM})..."

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

# 124 = timeout(1) mató QEMU. Es OK (el kernel se queda en idle).
if [ "$QEMU_EXIT" != "0" ] && [ "$QEMU_EXIT" != "124" ]; then
    log_error "QEMU salió con código $QEMU_EXIT"
    cat "$QEMU_LOG" >&2
    dump_serial force
    exit 1
fi

if [ ! -s "$SERIAL_LOG" ]; then
    log_error "serial.log está vacío"
    cat "$QEMU_LOG" >&2
    dump_serial force
    exit 1
fi

# ---------------------------------------------------------------------------
# Detectar si los APs arrancaron
# ---------------------------------------------------------------------------
aps_ok=0
if grep -qF "[SMP] APs listos y operativos" "$SERIAL_LOG"; then
    aps_ok=1
    n_aps=$(grep -oE '[0-9]+/[0-9]+ APs listos' "$SERIAL_LOG" | tail -1 || echo "?")
    log_info "APs arrancados: $n_aps"
else
    log_info "APs NO arrancados (esperado si QEMU_SMP=1 o TCG sin mailbox ACPI)"
fi

# ---------------------------------------------------------------------------
# Cadenas esperadas
# ---------------------------------------------------------------------------
log_info "Verificando cadenas de arranque..."

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
check_block "shell"    "${EXPECTED_SHELL[@]}"

# ---------------------------------------------------------------------------
# Verificar el bloque de tests
# ---------------------------------------------------------------------------
log_info "  Bloque: tests"

if grep -qE "\[TEST\] [0-9]+ tests:" "$SERIAL_LOG"; then
    tests_line=$(grep -oE "\[TEST\] [0-9]+ tests: [0-9]+ passed, [0-9]+ failed, [0-9]+ skipped" "$SERIAL_LOG" | tail -1)
    log_ok "$tests_line"

    passed=$(echo "$tests_line" | grep -oE "[0-9]+ passed" | grep -oE "[0-9]+")
    failed=$(echo "$tests_line" | grep -oE "[0-9]+ failed" | grep -oE "[0-9]+")
    skipped=$(echo "$tests_line" | grep -oE "[0-9]+ skipped" | grep -oE "[0-9]+")

    log_info "  passed=$passed failed=$failed skipped=$skipped"

    if [ "$failed" != "0" ]; then
        log_fail "Hay $failed tests fallidos"
        FAILED=$((FAILED + 1))
    fi

    if [ "$REQUIRE_SMP" = "yes" ]; then
        if [ "$skipped" != "0" ]; then
            log_fail "REQUIRE_SMP=yes pero hay $skipped tests skipped"
            FAILED=$((FAILED + 1))
        fi
    else
        if [ "$skipped" != "0" ]; then
            log_warn "Hay $skipped tests skipped (OK porque REQUIRE_SMP=no)"
        fi
    fi
else
    log_fail "No aparece el bloque '[TEST] N tests:'"
    FAILED=$((FAILED + 1))
fi

# ---------------------------------------------------------------------------
# Verificar SMP si REQUIRE_SMP=yes
# ---------------------------------------------------------------------------
if [ "$REQUIRE_SMP" = "yes" ]; then
    log_info "  Bloque: SMP (REQUIRE_SMP=yes)"
    if [ "$aps_ok" = "1" ]; then
        log_ok "APs arrancados"
    else
        log_fail "REQUIRE_SMP=yes pero los APs no arrancaron"
        FAILED=$((FAILED + 1))
    fi
fi

# ---------------------------------------------------------------------------
# Reporte final
# ---------------------------------------------------------------------------
if [ "$FAILED" -gt 0 ]; then
    echo ""
    log_error "$FAILED cadenas no encontradas o verificaciones fallidas"
    dump_serial force
    exit 1
fi

log_info "Todas las verificaciones pasaron. ${C_GRN}OK${C_RST}."

# Volcar el serial al final (si SHOW_SERIAL lo permite).
dump_serial

exit 0
