#!/bin/sh
# Tiempos de arranque de Wine con distintas opciones de GPU: escritorio
# (aurora-desktop.exe -> LISTO) y un proceso sin ventanas (cmd /c exit).
export HOME=/tmp/winehome WINEPREFIX=/tmp/winehome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
export AURORA_WINE_LOG=/tmp/aurora-wine.log WINEDEBUG=-all
mkdir -p "$HOME"
echo "WINE-SPEED: inicio"
sleep ${WAIT_DESKTOP:-90}
if [ ! -S /run/user/0/wayland-0 ]; then
  mkdir -p /run/user/0
  /usr/bin/aurora-wayland > /tmp/aw.log 2>&1 &
  sleep 8
fi
timeout 900 aurora-wine --setup </dev/null >/dev/null 2>&1; echo "WINE-SPEED: setup rc=$?"
SHARE=/usr/share/aurora/wine
medir() { # nombre
  wineserver -k 2>/dev/null; timeout 30 wineserver -w 2>/dev/null; sleep 2
  t0=$(date +%s%N)
  timeout 400 wine cmd /c exit >/dev/null 2>&1
  t1=$(date +%s%N)
  : > /tmp/esc.txt
  wine $SHARE/aurora-desktop.exe > /tmp/esc.txt 2>/dev/null &
  n=0; while [ ! -s /tmp/esc.txt ] && [ $n -lt 4000 ]; do sleep 0.1; n=$((n+1)); done
  t2=$(date +%s%N); r=$(head -n 1 /tmp/esc.txt)
  timeout 400 wine cmd /c exit >/dev/null 2>&1
  t3=$(date +%s%N)
  echo "WINE-SPEED: $1 cmd-frio=$(( (t1-t0)/1000000 )) ms escritorio=$r $(( (t2-t1)/1000000 )) ms cmd-caliente=$(( (t3-t2)/1000000 )) ms"
}
medir base
( export VK_LOADER_DRIVERS_DISABLE='*'; medir sin-vulkan )
( export VK_LOADER_DRIVERS_DISABLE='*' __EGL_VENDOR_LIBRARY_FILENAMES=/nonexistent.json; medir sin-vulkan-ni-egl )
( export LIBGL_ALWAYS_SOFTWARE=1 VK_LOADER_DRIVERS_DISABLE='*nvidia*'; medir software )
wineserver -k 2>/dev/null
echo "WINE-SPEED: fin"
