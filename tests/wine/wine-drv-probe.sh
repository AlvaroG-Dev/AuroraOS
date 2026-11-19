#!/bin/sh
# Diagnostico del driver grafico de Wine (explorer /desktop, winex11/winewayland).
export HOME=/tmp/winehome WINEPREFIX=/tmp/winehome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$HOME"
echo "WINE-DRV: inicio"
sleep ${WAIT_DESKTOP:-60}
if [ ! -S /run/user/0/wayland-0 ]; then
  mkdir -p /run/user/0
  /usr/bin/aurora-wayland > /tmp/aw.log 2>&1 &
  sleep 8
fi
WINEDEBUG=-all timeout 600 aurora-wine --setup > /tmp/wine-setup.log 2>&1; echo "WINE-DRV: setup rc=$?"
wineserver -p 900
wine timeout.exe /t 1200 /nobreak > /tmp/wine-keep.log 2>&1 &
WINEDEBUG=+explorer,+win,err+all,fixme-all timeout 300 wine winemine > /tmp/wine-warm.log 2>&1 &
sleep 120
echo "WINE-DRV: ps"; ls /proc | grep -c '^[0-9]'; ps -eo pid,stat,args 2>/dev/null | grep -i 'wine\|exe' | grep -v grep | head -30
echo "WINE-DRV: reg"; timeout 120 wine reg query 'HKLM\System\CurrentControlSet\Control\Video' /s 2>&1 | head -30
echo "WINE-DRV: warm"; grep -v 'trace:win:\(NtUser\|send\|peek\|get_win\|win_set\|set_window_pos\|apply\)' /tmp/wine-warm.log | head -80
WINEDEBUG=+reg,+atom,+loaddll,+module,+x11drv,err+all,fixme-all timeout 200 wine notepad > /tmp/wine-drv.log 2>&1 &
sleep 120
echo "WINE-DRV: notepad"; grep -n -i "video\|atom\|winex11\|x11drv\|err:\|warn:module" /tmp/wine-drv.log | head -80
echo "WINE-DRV: SHOT1"
sleep 10
echo "WINE-DRV: fin"
