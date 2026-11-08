#!/bin/bash
# Prueba E2E del instalador en QEMU (UEFI): 1) arranca la ISO instaladora con un
# disco virtual vacio y ejecuta aurora-install; 2) arranca SOLO ese disco y
# comprueba que aurora-boot monta la raiz ext2 y llega al init; 3) e2fsck -fn en
# el host sobre la particion raiz.
# Uso: tests/installer/run_install_test.sh   (SKIP_BUILD=1 reutiliza la ISO)
set -e
cd "$(dirname "$0")/../.."
MEM=${QEMU_MEM:-4G}; DISK=build/install-test.img; L1=/tmp/inst1.log; L2=/tmp/inst2.log
if [ -z "${SKIP_BUILD:-}" ]; then
  printf '%s\n' 'aurora-install --disk /dev/sdb --yes --no-reboot > /dev/kmsg 2>&1; echo INSTALL_RC=$? > /dev/kmsg' > sysroot/system/boot-tests
  trap 'rm -f sysroot/system/boot-tests' EXIT
  SKIP_ENV=1 make installer-iso > /tmp/inst-build.log 2>&1 || { tail -20 /tmp/inst-build.log; exit 1; }
  rm -f sysroot/system/boot-tests; trap - EXIT
fi
rm -f "$DISK" "$L1" "$L2"; truncate -s 3G "$DISK"
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/inst_vars.fd
COMMON="-m $MEM -smp 2 -machine q35 -no-reboot -display none -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=build/inst_vars.fd"
echo "== fase 1: instalar"
timeout -s TERM ${T1:-900} qemu-system-x86_64 $COMMON -device ahci,id=ahci0 \
  -drive if=none,id=cd,format=raw,file=aurora-installer.iso,snapshot=on -device ide-hd,drive=cd,bus=ahci0.0 \
  -drive if=none,id=hd,format=raw,file="$DISK" -device ide-hd,drive=hd,bus=ahci0.1 \
  -serial file:$L1 -monitor none &
QP=$!
for i in $(seq 1 ${T1:-900}); do grep -aq "INSTALL_RC=" $L1 2>/dev/null && break; sleep 1; done
grep -a "aurora-install\|INSTALL_RC" $L1 | tail -15
kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true
grep -aq "INSTALL_RC=0" $L1 || { echo "FAIL: instalacion"; exit 1; }
echo "== fase 1b: e2fsck en el host (antes de arrancar el disco)"
START=$(sgdisk -i 2 "$DISK" | awk '/^First sector/{print $3}')
e2fsck -fn "$DISK?offset=$((START * 512))" || { echo "FAIL: e2fsck"; exit 1; }
echo "== fase 2: arrancar solo el disco"
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/inst_vars.fd
timeout -s TERM ${T2:-300} qemu-system-x86_64 $COMMON -device ahci,id=ahci0 \
  -drive if=none,id=hd,format=raw,file="$DISK" -device ide-hd,drive=hd,bus=ahci0.0 \
  -serial file:$L2 -monitor none &
QP=$!
for i in $(seq 1 ${T2:-300}); do grep -aq "EXECVE.*aurora-wayland\|aurora-boot\] no se\|aurora-boot\] falta\|pivot_root fallo" $L2 2>/dev/null && break; sleep 1; done
sleep 5
grep -a "aurora-boot\|pivot_root\|EXECVE.*init\|aurora-wayland" $L2 | head -12
kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true
grep -aq "aurora-boot\] raiz .* montada" $L2 && grep -aq "EXECVE.*aurora-wayland" $L2 || { echo "FAIL: arranque desde disco"; exit 1; }
echo "PASS: instalador E2E"
