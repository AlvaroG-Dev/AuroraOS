#!/bin/bash
# Apps X11 (xterm, xeyes, xclock) sobre Xwayland como ventanas de Aurora.
# El host (scripts/qemu-drive.py) captura la pantalla en cada marcador SHOT-*.
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export DISPLAY=:0
export AW_DEBUG=1
export XD_INTERACTIVE="${XD_INTERACTIVE:-}"
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do
  [ -e /tmp/.X11-unix/X0 ] && break
  sleep 2
done
sleep 5
if [ -n "$XD_INTERACTIVE" ]; then
  # xterm con shell interactivo: el host hace clic y teclea (QEMU sendkey).
  /usr/bin/xterm -geometry 60x12 -e /bin/sh >/tmp/xterm.out 2>&1 &
else
  /usr/bin/xterm -geometry 60x12 -e /bin/sh -c 'echo X11-sobre-Aurora; sleep 600' >/tmp/xterm.out 2>&1 &
fi
/usr/bin/xeyes >/tmp/xeyes.out 2>&1 &
/usr/bin/xclock >/tmp/xclock.out 2>&1 &
sleep ${XD_WAIT:-60}
echo "[x11-demo.sh] SHOT-1"
sleep ${XD_WAIT:-60}
echo "[x11-demo.sh] SHOT-2"
sleep 10
echo "== xtran"; ls -la /tmp/xtran 2>&1; cat /tmp/xtran 2>&1 | head -3
for f in xterm xeyes xclock; do echo "== $f.out"; head -5 /tmp/$f.out; done
echo "=== aw.log"; grep -a "^\[aurora-wayland\]" /tmp/aw.log | head -80
echo DRIVE-DONE
