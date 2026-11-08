#!/bin/bash
# N5c: glxgears (GLX) sobre Xwayland con glamor y la RTX 4060. Uso: glx.sh [segundos=15]
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export __GLX_VENDOR_LIBRARY_NAME=nvidia
S=${1:-15}
{ echo "== xdpyinfo/glxinfo"; timeout 30 glxinfo -B 2>&1 | head -25; } >/tmp/glx.info 2>&1
timeout $S glxgears >/tmp/glx.out 2>&1
echo "GLX rc=$?" > /dev/kmsg
echo "GLX info: $(head -c 2500 /tmp/glx.info | tr '\n' '|')" > /dev/kmsg
echo "GLX out: $(head -c 1500 /tmp/glx.out | tr '\n' '|')" > /dev/kmsg
head -30 /tmp/glx.info; head -12 /tmp/glx.out
