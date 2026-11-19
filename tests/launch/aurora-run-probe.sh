#!/bin/sh
# aurora-run en el sistema: que reconoce cada tipo de programa y que execve de
# un .exe desde la shell acaba en Wine (el kernel lo pasa a aurora-run).
export HOME=/tmp/runhome WINEPREFIX=/tmp/runhome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$HOME/.config/aurora"
ok=0; bad=0
chk() { if [ "$1" = "$2" ]; then ok=$((ok+1)); echo "  ok   $3 -> $1"; else bad=$((bad+1)); echo "  FALLO $3: '$1' (esperado '$2')"; fi; }
kind() { aurora-run --que "$@" 2>&1 | sed -n 's/^[^:]*: \([a-z]*\) (.*/\1/p'; }
echo "RUN-TEST: inicio"
NOTEPAD=/usr/lib/wine/x86_64-windows/notepad.exe
cp "$NOTEPAD" /tmp/bloc.exe; chmod +x /tmp/bloc.exe
chk "$(kind /usr/bin/calculadora)" aurora "calculadora (nota .note.aurora)"
chk "$(kind terminal)" aurora "terminal por PATH"
chk "$(kind /usr/bin/bash)" linux "bash (glibc)"
chk "$(kind /usr/bin/aurora-wine)" linux "script #!"
chk "$(kind /tmp/bloc.exe)" windows "PE de 64 bits"
chk "$(kind /usr/lib/wine/i386-windows/winemine.exe)" windows "PE de 32 bits"
chk "$(kind /usr/share/applications/wine-notepad.desktop)" linux ".desktop (Exec=aurora-wine)"
chk "$(kind --con vm /tmp/bloc.exe)" vm "--con vm"
echo "bloc.exe vm" > "$HOME/.config/aurora/run.conf"
chk "$(kind /tmp/bloc.exe)" vm "regla de run.conf"
aurora-run /tmp/bloc.exe > /tmp/vm.log 2>&1; chk "$?" 126 "mini-VM aun no disponible (rc)"
grep -q 'mini-VM' /tmp/vm.log && chk 1 1 "aviso de mini-VM" || chk 0 1 "aviso de mini-VM"
rm -f "$HOME/.config/aurora/run.conf"
aurora-run --que /etc/aurora/run.conf > /dev/null 2>&1; chk "$?" 1 "fichero que no es programa"
echo "RUN-TEST: $ok ok, $bad fallos$([ $bad = 0 ] && echo ' ALL PASS')"

# execve de un .exe desde la shell -> kernel -> aurora-run --binfmt -> Wine.
sleep ${WAIT_DESKTOP:-60}
if [ ! -S /run/user/0/wayland-0 ]; then
  mkdir -p /run/user/0
  /usr/bin/aurora-wayland > /tmp/aw.log 2>&1 &
  sleep 8
fi
WINEDEBUG=-all timeout 600 aurora-wine --setup > /tmp/wine-setup.log 2>&1
# QEMU sin KVM: el escritorio de Wine se arranca antes (ver wine-gui-probe.sh).
wineserver -k 2>/dev/null; sleep 3; wineserver -p 900
wine explorer.exe /desktop > /dev/null 2>&1 &
sleep 20
wine timeout.exe /t 1200 /nobreak > /dev/null 2>&1 &
sleep 100
cd /tmp && AURORA_RUN_VERBOSE=1 ./bloc.exe > /tmp/bloc.log 2>&1 &
sleep 60
echo "== bloc.log"; head -5 /tmp/bloc.log
echo "RUN-TEST: SHOT1"
sleep 15
echo "RUN-TEST: fin"
