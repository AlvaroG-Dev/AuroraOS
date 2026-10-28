#!/bin/bash
# scripts/lkpi-regress.sh — bateria de regresion de LinuxKPI en QEMU (un solo
# QEMU a la vez, via scripts/qemu-drive.py). Uso:
#   scripts/lkpi-regress.sh {suite|lkpi_test|drm_test|userland|all}
# Sale con 1 si alguna prueba falla; resumen al final. Logs en /tmp/lkpi-*.log.
set -uo pipefail
cd "$(dirname "$0")/.."
D="python3 scripts/qemu-drive.py -m 6G"
FAILS=()

# run NOMBRE FALLO_REGEX EXITO_REGEX [args de qemu-drive]
run() {
    local name=$1 okre=$2; shift 2
    local log=/tmp/lkpi-$name.log
    rm -f "$log"
    echo "[lkpi-regress] $name ..."
    $D --log "$log" "$@" >/tmp/lkpi-$name.out 2>&1
    if grep -aqE "$okre" "$log" && ! grep -aE "\] FAIL|PANIC" "$log" | grep -qv OPEN-FAIL; then
        echo "[lkpi-regress] $name OK"
    else
        echo "[lkpi-regress] $name FALLA (ver $log)"; FAILS+=("$name")
    fi
}

suite() {
    run suite-smp2 "tests: [0-9]+ passed|[0-9]+ passed, 0 failed" -t 2400 --done "passed, "
    run suite-smp4 "passed, 0 failed" -t 2400 --done "passed, " --extra=-smp --extra=4
}
lkpi_test() { run lkpi_test "LKPI-TEST: ALL PASS" -t 1500 --extra=-smp --extra=4 --boot-test /usr/bin/lkpi_test --done "LKPI-TEST:"; }
drm_test() {
    run drm-native "ALL PASS" -t 900 --boot-test /usr/bin/drm_test --done "ALL PASS"
    run drm-bochs "ALL PASS" -t 900 --cmdline "aur.skip=1234:1111 lkpi.load=linuxkpi,lkpi_drm,lkpi_bochs" --boot-test /usr/bin/drm_test --done "ALL PASS"
    run drm-simpledrm "ALL PASS" -t 900 --cmdline "lkpi.load=linuxkpi,lkpi_drm,lkpi_simpledrm" --boot-test /usr/bin/drm_test --done "ALL PASS"
    run drm-virtio-gpu "ALL PASS" -t 900 --cmdline "aur.skip=1af4:1050 lkpi.load=linuxkpi,lkpi_virtio,lkpi_drm,lkpi_virtio_gpu" \
        --extra=-vga --extra=none --extra=-device --extra=virtio-gpu-pci,max_outputs=2 --boot-test /usr/bin/drm_test --done "ALL PASS"
}
userland() {
    for t in sig_test rseq_test timer_test thread_sig_test; do
        run $t "ALL PASS" -t 600 --boot-test /usr/bin/$t --done "ALL PASS"
    done
    run clipboard "WAYLAND->X11 xclip -o = 'de-wayland-123'.*" -t 600 --boot-test /usr/bin/clipboard-x11.sh --done DRIVE-DONE
    run primary-icons "SEPARADOS primary='prim-de-x11-456' clipboard='solo-clipboard'" -t 600 --boot-test /usr/bin/primary-icons.sh --done DRIVE-DONE
}

case "${1:-all}" in
    suite) suite ;; lkpi_test) lkpi_test ;; drm_test) drm_test ;; userland) userland ;;
    all) suite; lkpi_test; drm_test; userland ;;
    *) echo "uso: $0 {suite|lkpi_test|drm_test|userland|all}"; exit 2 ;;
esac
if [ ${#FAILS[@]} -gt 0 ]; then echo "[lkpi-regress] FALLAN: ${FAILS[*]}"; exit 1; fi
echo "[lkpi-regress] todo OK"
