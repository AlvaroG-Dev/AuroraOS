#!/bin/bash
# Portapapeles X11 <-> Wayland a traves de Xwayland:
#   wl-copy (Wayland) -> xclip -o (X11)   y   xclip -i (X11) -> wl-paste (Wayland)
# Hay una ventana X11 (xclock) abierta: es la que tiene el foco y por la que
# Xwayland recibe las selecciones. El host captura la pantalla en SHOT-1 (Dock).
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export DISPLAY=:0
export AW_DEBUG=1
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do
  [ -e /tmp/.X11-unix/X0 ] && break
  sleep 2
done
sleep 5
/usr/bin/xclock >/dev/null 2>&1 &
/usr/bin/xterm -geometry 40x6 -e /bin/sh -c 'sleep 600' >/dev/null 2>&1 &
sleep ${CX_WAIT:-40}

# 1. Wayland -> X11
echo -n "de-wayland-123" | /usr/bin/wl-copy >/tmp/copy1.out 2>&1
sleep 3
R1=$(timeout 20 /usr/bin/xclip -o -selection clipboard 2>/tmp/x1.err)
echo "[clipboard-x11.sh] WAYLAND->X11 xclip -o = '$R1' (esperado 'de-wayland-123') $(cat /tmp/x1.err)"

# 2. X11 -> Wayland
echo -n "de-x11-456" | /usr/bin/xclip -i -selection clipboard >/tmp/x2.out 2>&1
sleep 3
R2=$(timeout 20 /usr/bin/wl-paste -n 2>/tmp/p2.err)
echo "[clipboard-x11.sh] X11->WAYLAND wl-paste = '$R2' (esperado 'de-x11-456') $(cat /tmp/p2.err)"

echo "[clipboard-x11.sh] SHOT-1"
sleep ${CX_HOLD:-25}
kill -TERM $APID 2>/dev/null
sleep 2
echo "=== aw.log"; grep -a -e seleccion -e set_selection -e "kbd focus" -e toplevel -e "X11 0x" /tmp/aw.log | head -60
echo DRIVE-DONE
