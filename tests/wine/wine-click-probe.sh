#!/bin/sh
# Clic en una ventana de Wine: el host (qemu-drive.py) mueve el raton y pulsa
# en el Buscaminas entre SHOT1 y "despues". La segunda instancia del
# Buscaminas lleva WINEDEBUG=+event para ver si a Wine le llega el ButtonPress.
export HOME=/tmp/winehome WINEPREFIX=/tmp/winehome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
export AURORA_WINE_LOG=/tmp/aurora-wine.log
mkdir -p "$HOME"
echo "WINE-CLICK: inicio"
sleep ${WAIT_DESKTOP:-90}
if [ ! -S /run/user/0/wayland-0 ]; then
  mkdir -p /run/user/0
  AW_DEBUG=1 /usr/bin/aurora-wayland > /tmp/aw.log 2>&1 &
  sleep 8
fi
timeout 900 aurora-wine --setup </dev/null >/dev/null 2>&1; echo "WINE-CLICK: setup rc=$?"
AURORA_WINE_APP_DEBUG=err+all,+event,+cursor AURORA_WINE_LOG=/tmp/ev.log aurora-wine winemine </dev/null >/dev/null 2>&1 &
sleep ${WAIT_APP:-600}
echo "WINE-CLICK: SHOT1"
sleep ${WAIT_CLICK:-150}
echo "WINE-CLICK: despues"
grep -a "aurora-wine:" /tmp/ev.log | tail -n 10
echo "== eventos"; grep -ai "ButtonPress\|ButtonRelease\|EnterNotify\|FocusIn\|FocusOut\|ClientMessage\|ConfigureNotify\|MapNotify" /tmp/ev.log | head -n 60
echo "== motion $(grep -aci MotionNotify /tmp/ev.log) lineas $(wc -l < /tmp/ev.log)"
grep -a "err:" /tmp/ev.log | head -20
echo "== aw.log"; grep -a "ptr win\|X11\|toplevel" /tmp/aw.log | tail -n 25
echo "WINE-CLICK: fin"
