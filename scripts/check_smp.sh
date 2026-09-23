#!/usr/bin/env bash
# scripts/check_smp.sh
#
# Ejecuta la regresión de boot de Aurora-OS con 2 y 4 CPUs.
#
# IMPORTANTE:
#   Este script NO añade ni inventa tests de funcionalidades SMP nuevas.
#   Solo fuerza QEMU a arrancar con 2/4 CPUs y hace que check_boot.sh exija
#   que los APs reales arranquen. Los tests de kernel que ya están registrados
#   y marcados TEST_FLAG_NEEDS_SMP se ejecutarán cuando haya APs disponibles.
#
# Uso:
#   ./scripts/check_smp.sh
#   TIMEOUT=180 ./scripts/check_smp.sh
#   USE_KVM=no ./scripts/check_smp.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TIMEOUT="\${TIMEOUT:-120}"
SHOW_SERIAL="\${SHOW_SERIAL:-on-failure}"
REQUIRE_SMP=yes

for cpus in 2 4; do
    echo ""
    echo "============================================================"
    echo " Aurora-OS SMP boot check: \${cpus} CPUs"
    echo "============================================================"
    echo ""

    TIMEOUT="$TIMEOUT" \
    SHOW_SERIAL="$SHOW_SERIAL" \
    REQUIRE_SMP="$REQUIRE_SMP" \
    ./scripts/check_boot.sh --smp "$cpus"

    echo ""
    echo "[SMP] \${cpus} CPUs: OK"
done

echo ""
echo "[SMP] Checks de 2 y 4 CPUs completados correctamente."
