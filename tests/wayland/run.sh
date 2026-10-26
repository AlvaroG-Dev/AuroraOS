#!/bin/bash
# Encadena los hitos Wayland/ABI en QEMU y falla si alguno no imprime ALL PASS.
# Uso (dentro de WSL/Linux):  tests/wayland/run.sh
set -u
cd "$(dirname "$0")/../.."
bash tests/wayland/build.sh || exit 1

TESTS=(sig_test unix_test dev_test timerfd_test shm_test wl_test udev_test evdev_test drm_test)
CMDS=()
for t in "${TESTS[@]}"; do
  case "$t" in
    evdev_test) CMDS+=("XDG_RUNTIME_DIR=/run/user/0 /usr/bin/$t wait") ;;
    *) CMDS+=("XDG_RUNTIME_DIR=/run/user/0 /usr/bin/$t") ;;
  esac
done

# Herramientas DRM de la distro (sin tocar), si estan en el sysroot.
TOOLS=0
if [ -x precompiled/usr/bin/modetest ] && [ -x precompiled/usr/bin/drm_info ]; then
  TOOLS=1
  CMDS+=("/usr/bin/modetest -M aurora 2>&1" "/usr/bin/drm_info 2>&1")
fi

# evdev_test imprime READY y entonces se inyectan teclado y raton por el monitor.
QMON="sendkey a;mouse_move 30 12;mouse_move 8 5" QMON_WAIT="evdev_test] READY" \
  bash scripts/qemu-boot-test.sh -t 180 "${CMDS[@]}"

rc=0
for t in "${TESTS[@]}"; do
  if grep -aq "\[$t\] ALL PASS" /tmp/boot-test.log; then echo "PASS $t"; else echo "FAIL $t"; rc=1; fi
done
if [ "$TOOLS" = 1 ]; then
  if grep -aq "opened device .AuroraOS DRM/KMS. on driver .aurora" /tmp/boot-test.log; then echo "PASS modetest"; else echo "FAIL modetest"; rc=1; fi
  if grep -aq "Node: /dev/dri/card0" /tmp/boot-test.log; then echo "PASS drm_info"; else echo "FAIL drm_info"; rc=1; fi
fi
grep -a "\[TEST\] [0-9]* tests:" /tmp/boot-test.log
exit $rc
