#!/bin/bash
# Prueba E2E del instalador GRAFICO en QEMU (UEFI): arranca aurora-installer.iso con un
# disco vacio, maneja el asistente con el teclado (monitor de QEMU), instala, hace
# capturas de cada paso en build/gui-shots/ y arranca despues solo el disco.
# Uso: tests/installer/run_gui_test.sh [--no-build]
set -e
cd "$(dirname "$0")/../.."
MEM=${QEMU_MEM:-4G}; DISK=build/install-test.img; SHOTS=build/gui-shots; L1=/tmp/gui-inst.log; L2=/tmp/gui-boot.log
SOCK=/tmp/qmon-gui.sock
[ "$1" = "--no-build" ] || SKIP_ENV=1 make installer-iso > /tmp/inst-build.log 2>&1 || { tail -20 /tmp/inst-build.log; exit 1; }
mkdir -p $SHOTS; rm -f "$DISK" "$L1" "$L2" $SOCK $SHOTS/*; truncate -s 3G "$DISK"
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/inst_vars.fd
COMMON="-m $MEM -smp 2 -machine q35 -no-reboot -display none -vga std -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=build/inst_vars.fd"
timeout -s TERM ${T1:-1800} qemu-system-x86_64 $COMMON \
  -drive if=none,id=cd,format=raw,file=aurora-installer.iso,snapshot=on -device ahci,id=ahci0 -device ide-cd,drive=cd,bus=ahci0.0,bootindex=0 \
  -drive if=none,id=hd,format=raw,file="$DISK" -device ide-hd,drive=hd,bus=ahci0.1 \
  -nic user,model=e1000e -serial file:$L1 -monitor unix:$SOCK,server,nowait &
QP=$!
export SOCK SHOTS
mon() { python3 - "$@" <<'PY'
import socket,sys,time,os
s=socket.socket(socket.AF_UNIX)
for _ in range(20):
    try: s.connect(os.environ['SOCK']); break
    except Exception: time.sleep(0.5)
time.sleep(0.3)
for c in sys.argv[1:]:
    if c.startswith('shot:'):
        s.send(('screendump %s/%s.ppm\n' % (os.environ['SHOTS'], c[5:])).encode()); time.sleep(2)
        try:
            from PIL import Image
            Image.open('%s/%s.ppm' % (os.environ['SHOTS'], c[5:])).save('%s/%s.png' % (os.environ['SHOTS'], c[5:])); os.remove('%s/%s.ppm' % (os.environ['SHOTS'], c[5:]))
        except Exception: pass
    elif c.startswith('type:'):
        for ch in c[5:]:
            k = {'_':'shift-minus','-':'minus'}.get(ch, ('shift-'+ch.lower()) if ch.isupper() else ch)
            s.send(('sendkey %s\n' % k).encode()); time.sleep(0.35)
    else:
        s.send((c+'\n').encode()); time.sleep(0.5)
PY
}
# Esperar a que el instalador pinte (init lanza la app) y avanzar el asistente.
for i in $(seq 1 600); do grep -aq "Cargando /usr/bin/init" $L1 2>/dev/null && break; sleep 1; done
sleep 40
mon shot:1-bienvenida "sendkey ret"; sleep 3
mon shot:2-disco "sendkey ret"; sleep 2
mon "type:Tester" "sendkey tab" "sendkey tab" "type:secret1" "sendkey tab" "type:secret1"; sleep 1
mon shot:3-cuenta "sendkey ret"; sleep 2
mon shot:4-extras "sendkey ret"; sleep 2
mon shot:5-confirmar "sendkey ret"; sleep 20
mon shot:6-instalando
for i in $(seq 1 ${T1:-1800}); do grep -aq "Instalacion completada\|ERROR:" $L1 2>/dev/null && break; sleep 2; done
sleep 8
mon shot:7-listo
grep -a "aurora-install\]" $L1 | tail -14
kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true
grep -aq "Instalacion completada" $L1 || { echo "FAIL: instalacion"; exit 1; }
echo "== e2fsck en el host (raiz y datos)"
for n in 2 3; do START=$(sgdisk -i $n "$DISK" | awk '/^First sector/{print $3}'); e2fsck -fn "$DISK?offset=$((START * 512))" || { echo "FAIL: e2fsck p$n"; exit 1; }; done
echo "== arranque solo del disco"
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/inst_vars.fd
timeout -s TERM ${T2:-400} qemu-system-x86_64 $COMMON -device ahci,id=ahci0 \
  -drive if=none,id=hd,format=raw,file="$DISK" -device ide-hd,drive=hd,bus=ahci0.0 \
  -serial file:$L2 -monitor unix:$SOCK,server,nowait &
QP=$!
for i in $(seq 1 ${T2:-400}); do grep -aq "EXECVE.*aurora-wayland" $L2 2>/dev/null && break; sleep 1; done
sleep 30
mon shot:8-escritorio
grep -a "aurora-boot\|pivot\|EXECVE.*\(init\|aurora-wayland\|terminal\)\|login\|/data" $L2 | head -14 | cut -c1-180
kill $QP 2>/dev/null || true; wait $QP 2>/dev/null || true
grep -aq "aurora-boot\] raiz .* montada" $L2 && grep -aq "EXECVE.*aurora-wayland" $L2 || { echo "FAIL: arranque desde disco"; exit 1; }
echo "PASS: instalador grafico E2E (capturas en $SHOTS)"
