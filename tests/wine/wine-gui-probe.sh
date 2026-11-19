#!/bin/sh
# Wine en el escritorio de Aurora: prefijo nuevo con aurora-wine y apps
# Windows (Bloc de notas, winecfg, Buscaminas) como ventanas de Aurora
# (Xwayland dentro de aurora-wayland). La captura la hace qemu-drive.py.
export HOME=/tmp/winehome WINEPREFIX=/tmp/winehome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
export WINEDEBUG=${WINEDEBUG:-err+all,fixme-all}
mkdir -p "$HOME"
echo "WINE-GUI: inicio $(date +%s)"
sleep ${WAIT_DESKTOP:-90}
# En modo de pruebas init no arranca aurora-wayland (ni Xwayland con él).
if [ ! -S /run/user/0/wayland-0 ]; then
  mkdir -p /run/user/0
  /usr/bin/aurora-wayland > /tmp/aw.log 2>&1 &
  sleep 8
fi
ls -la /run/user/0 /tmp/.X11-unix 2>&1 | head
export AURORA_WINE_LOG=/tmp/wine-setup.log
t0=$(date +%s)
timeout 900 aurora-wine --setup > /tmp/wine-setup.log 2>&1; echo "WINE-GUI: setup rc=$? en $(( $(date +%s) - t0 )) s"
grep -v ":fixme" /tmp/wine-setup.log | sort | uniq -c | sort -rn | head -40
ls "$WINEPREFIX/drive_c/windows/system32" 2>/dev/null | wc -l
grep -c . "$WINEPREFIX/user.reg" 2>/dev/null
grep -A3 'Control Panel\\\\Colors' "$WINEPREFIX/user.reg" | head -5
timeout 60 wine cmd /c "echo HOLA-DESDE-WINDOWS & ver" 2>&1 | tail -3
# En QEMU sin KVM el explorer.exe que hace de escritorio tarda mas de los 10 s
# que win32u espera al arrancarlo: se arranca antes, a mano, y se le deja
# terminar. timeout.exe mantiene el escritorio abierto entre una app y otra.
wineserver -k 2>/dev/null; sleep 3
wineserver -p 900
wine explorer.exe /desktop > /tmp/wine-warm.log 2>&1 &
sleep 20
wine timeout.exe /t 1200 /nobreak > /tmp/wine-keep.log 2>&1 &
sleep ${WAIT_EXPLORER:-100}
aurora-wine notepad > /tmp/wine-notepad.log 2>&1 &
sleep 20
aurora-wine winemine > /tmp/wine-mine.log 2>&1 &
sleep 20
aurora-wine winecfg > /tmp/wine-cfg.log 2>&1 &
sleep 60
echo "WINE-GUI: SHOT1"
sleep 15
# Modo claro de Aurora (en el live /data es de solo lectura: AURORA_THEME). Los
# colores del sistema de Wine se leen al arrancar wineserver: se reinicia.
export AURORA_THEME=light
wineserver -k 2>/dev/null; sleep 5
wineserver -p 900
wine explorer.exe /desktop > /tmp/wine-warm2.log 2>&1 &
sleep 20
wine timeout.exe /t 1200 /nobreak > /tmp/wine-keep2.log 2>&1 &
sleep ${WAIT_EXPLORER:-100}
aurora-wine notepad > /tmp/wine-notepad2.log 2>&1 &
sleep 40
aurora-wine winecfg > /tmp/wine-cfg2.log 2>&1 &
sleep 70
echo "WINE-GUI: SHOT2"
sleep 15
for f in /tmp/wine-warm.log /tmp/wine-notepad.log /tmp/wine-mine.log /tmp/wine-cfg.log; do echo "== $f"; tail -n 8 $f; done
echo "== aw.log"; tail -n 30 /tmp/aw.log
echo "WINE-GUI: fin"
