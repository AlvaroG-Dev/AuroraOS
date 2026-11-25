#!/bin/sh
# Wine lanzado en frio, como desde el Dock: prefijo ya creado, sin wineserver
# ni explorer.exe en marcha, solo `aurora-wine winemine`. Sin los atajos de
# wine-gui-probe.sh (que arranca antes el escritorio a mano).
export HOME=/tmp/winehome WINEPREFIX=/tmp/winehome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
export AURORA_WINE_LOG=/tmp/aurora-wine.log
mkdir -p "$HOME"
echo "WINE-DOCK: inicio"
sleep ${WAIT_DESKTOP:-90}
if [ ! -S /run/user/0/wayland-0 ]; then
  mkdir -p /run/user/0
  /usr/bin/aurora-wayland > /tmp/aw.log 2>&1 &
  sleep 8
fi
t0=$(date +%s)
timeout 900 aurora-wine --setup </dev/null >/dev/null 2>&1; echo "WINE-DOCK: setup rc=$? en $(( $(date +%s) - t0 )) s"
wineserver -k 2>/dev/null; sleep 5
t0=$(date +%s)
WINEDEBUG=${DOCK_DEBUG:-err+all,warn+all,fixme-all,+timestamp,+explorer,+system,+win,+process} aurora-wine winemine </dev/null >/dev/null 2>&1 &
sleep ${WAIT_APP:-240}
echo "WINE-DOCK: SHOT1 ($(( $(date +%s) - t0 )) s)"
sleep 15
ps 2>/dev/null | grep -i wine | head -20
echo "== aurora-wine.log"; grep -a "explorer\|:system:\|aurora-wine:\|err:\|nodrv\|desktop\|NCCREATE\|:win:" /tmp/aurora-wine.log | grep -v "egl_init\|keyboard\|vulkan" | tail -n 400; grep -c . /tmp/aurora-wine.log
echo "== aw.log"; tail -n 30 /tmp/aw.log 2>/dev/null
echo "WINE-DOCK: fin"
