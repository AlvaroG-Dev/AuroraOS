#!/bin/sh
# Estilo visual de Aurora en Wine y posicion de los menus: el host
# (qemu-drive.py) abre el menu Game del Buscaminas entre SHOT1 y SHOT2 y hace
# capturas; despues se abre winecfg (pestanas, botones, casillas, listas).
export HOME=/tmp/winehome WINEPREFIX=/tmp/winehome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
export AURORA_WINE_LOG=/tmp/aurora-wine.log
mkdir -p "$HOME"
echo "WINE-THEME: inicio"
sleep ${WAIT_DESKTOP:-90}
if [ ! -S /run/user/0/wayland-0 ]; then
  mkdir -p /run/user/0
  AW_DEBUG=1 /usr/bin/aurora-wayland > /tmp/aw.log 2>&1 &
  sleep 8
fi
timeout 900 aurora-wine --setup </dev/null >/dev/null 2>&1; echo "WINE-THEME: setup rc=$?"
wine reg query 'HKCU\Software\Microsoft\Windows\CurrentVersion\ThemeManager' 2>/dev/null | tr -d '\r'
aurora-wine winemine </dev/null >/dev/null 2>&1 &
sleep ${WAIT_APP:-420}
echo "WINE-THEME: SHOT1"
sleep ${WAIT_MENU:-120}
aurora-wine winecfg </dev/null >/dev/null 2>&1 &
sleep ${WAIT_CFG:-300}
echo "WINE-THEME: SHOT2"
sleep 40
echo "== aw.log"; grep -a "popup\|override" /tmp/aw.log | tail -n 15
grep -a "err:\|uxtheme" /tmp/aurora-wine.log | tail -n 15
echo "WINE-THEME: fin"
