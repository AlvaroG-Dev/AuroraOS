#!/bin/bash
# Dentro de AuroraOS: ejecuta libinput-debug-events (el de la distro, sin
# tocar) sobre los dos evdev durante $1 segundos (25 por defecto) y lo mata.
/usr/bin/libinput-debug-events --device /dev/input/event0 --device /dev/input/event1 &
P=$!
sleep "${1:-25}"
kill "$P"
