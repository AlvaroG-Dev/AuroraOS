#!/bin/bash
# scripts/fetch-xwayland.sh — Xwayland, xkbcomp y unas apps X11 de prueba (xterm,
# xeyes, xclock) de Ubuntu, sin tocar, para correr aplicaciones X11 como ventanas
# de Aurora (aurora-wayland hace de gestor de ventanas, ver "Xwayland" en
# Plan_Wayland_Unificado.md). Requiere en el host de build (Ubuntu 24.04):
#   apt-get install xwayland x11-apps xterm x11-xkb-utils xkb-data xclip xsel
#                   libxcb1-dev libxcb-composite0-dev
# Copia binarios, datos de xkb y el cierre de librerias que falte en libs/glibc.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBDIR="$ROOT/libs/glibc/lib/x86_64-linux-gnu"
PRE="$ROOT/precompiled"
G=/usr/lib/x86_64-linux-gnu
mkdir -p "$PRE/usr/bin" "$PRE/usr/share/X11" "$PRE/usr/share/applications" "$LIBDIR"

BINS="Xwayland xkbcomp xterm xeyes xclock xclip xsel"
for b in $BINS; do
    cp -Lf "/usr/bin/$b" "$PRE/usr/bin/$b" && echo "  bin: $b"
done
# teclado: Xwayland compila el keymap con xkbcomp contra /usr/share/X11/xkb
rm -rf "$PRE/usr/share/X11/xkb"
cp -a /usr/share/X11/xkb "$PRE/usr/share/X11/xkb"
# xkeyboard-config puede ser mas nuevo que el xkbcomp copiado y nombrar keysyms
# que este no conoce (XF86CameraAccess*, XF86NavChart...): xkbcomp avisa de cada
# uno en el arranque de Xwayland. Se compila un keymap con el xkbcomp copiado y
# se quitan de symbols/inet las teclas cuyo keysym no resuelve.
XKB="$PRE/usr/share/X11/xkb"
printf 'xkb_keymap {\n xkb_keycodes { include "evdev+aliases(qwerty)" };\n xkb_types { include "complete" };\n xkb_compat { include "complete" };\n xkb_symbols { include "pc+us+inet(evdev)" };\n};\n' > /tmp/aurora-km.xkb
"$PRE/usr/bin/xkbcomp" -w 1 "-R$XKB" -xkm - /tmp/aurora-km.xkm < /tmp/aurora-km.xkb 2>&1 |
    sed -n 's/.*resolve keysym \([A-Za-z0-9_]*\).*/\1/p' | sort -u | while read -r k; do
        sed -i "/\[ $k[ ]*\]/d" "$XKB/symbols/inet"
    done
[ -d /usr/share/X11/app-defaults ] && cp -a /usr/share/X11/app-defaults "$PRE/usr/share/X11/" || true

mkdesktop() { # id Nombre ejecutable
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=/usr/bin/%s\nStartupWMClass=%s\n' "$2" "$3" "$3" \
        > "$PRE/usr/share/applications/$1.desktop"
}
mkdesktop xterm "XTerm" xterm
mkdesktop xeyes "XEyes" xeyes
mkdesktop xclock "XClock" xclock

ELFS=""
for b in $BINS; do ELFS="$ELFS $PRE/usr/bin/$b"; done
export LD_LIBRARY_PATH="$LIBDIR:$G"
for pass in 1 2 3; do
    for f in $ELFS; do
        ldd "$f" 2>/dev/null | awk '/=> \// {print $3}'
    done | sort -u | while read -r l; do
        b="$(basename "$l")"
        if [ ! -e "$LIBDIR/$b" ] && [ ! -L "$LIBDIR/$b" ]; then
            cp -Lf "$l" "$LIBDIR/$b" && echo "  lib: $b"
        fi
    done
    ELFS="$ELFS $(ls "$LIBDIR"/*.so* 2>/dev/null | head -1000)"
done
# libgallium / libLLVM (Mesa) no se versionan ni se copian aqui: las trae
# scripts/fetch-mesa.sh si hace falta GL (Xwayland va por wl_shm con -shm).
rm -f "$LIBDIR"/libLLVM* "$LIBDIR"/libgallium-* 2>/dev/null || true
echo OK
