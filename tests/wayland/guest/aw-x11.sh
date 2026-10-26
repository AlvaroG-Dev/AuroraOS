#!/bin/bash
# Prueba de apps X11 sobre Xwayland: espera a que aurora-wayland (que arranca
# Xwayland al inicio) deje el socket del display y lanza xterm, xeyes y xclock.
export DISPLAY=:0
for _ in $(seq 1 90); do
  [ -S /tmp/.X11-unix/X0 ] && break
  sleep 2
done
sleep 5
/usr/bin/xterm -geometry 60x12 -e 'echo X11-sobre-Aurora; sleep 600' &
/usr/bin/xeyes &
/usr/bin/xclock &
wait
