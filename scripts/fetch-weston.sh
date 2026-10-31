#!/bin/bash
# scripts/fetch-weston.sh — Descarga de Ubuntu (sin compilar ni modificar) Weston,
# foot, libseat y las librerias que necesitan, y las deja en el sysroot:
#   - librerias  -> libs/glibc/lib/x86_64-linux-gnu/
#   - binarios   -> precompiled/usr/bin/
#   - plugins    -> precompiled/usr/lib/x86_64-linux-gnu/{libweston-13,weston}/
#   - datos      -> precompiled/usr/share/{weston,fonts,glvnd,...}
# Primero solo render por software (pixman): sin LLVM/llvmpipe. Mesa GL
# (libgbm/libEGL) se instala porque libweston enlaza contra ellas.
#
# Uso:  scripts/fetch-weston.sh [paquete-extra ...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/build/weston_stage"
DEBS="$STAGE/debs"
X="$STAGE/root"
LIBDIR="$ROOT/libs/glibc/lib/x86_64-linux-gnu"
PRE="$ROOT/precompiled"
MULTI=usr/lib/x86_64-linux-gnu

PKGS=(
    weston libweston-13-0 libweston-13-dev- libseat1 libdisplay-info1 liblcms2-2
    libgbm1 libegl1 libglvnd0 libgles2 libglapi-mesa libegl-mesa0
    libxcb-composite0 libxcursor1
    foot libfcft4t64 libutf8proc3 libtllist-dev- fonts-dejavu-core fontconfig-config
    libfontconfig1 libfreetype6 libharfbuzz0b libpng16-16t64 libcairo2
    libpango-1.0-0 libpangocairo-1.0-0 libjpeg8 libwebp7 libsystemd0
    ncurses-term
    "$@"
)

mkdir -p "$DEBS" "$X" "$LIBDIR" "$PRE/usr/bin"
cd "$DEBS"
for p in "${PKGS[@]}"; do
    case "$p" in *-) continue ;; esac
    ls "${p}"_*.deb >/dev/null 2>&1 || apt-get download "$p" >/dev/null 2>&1 \
        || echo "  (sin paquete: $p)"
done
for d in *.deb; do dpkg -x "$d" "$X"; done

# Iterar: resolver con ldd lo que falte y bajar el paquete que lo contiene.
export LD_LIBRARY_PATH="$X/$MULTI:$X/lib/x86_64-linux-gnu:$X/$MULTI/weston:$LIBDIR"
export WSTAGE="$X"
bins=("$X"/usr/bin/weston "$X"/usr/bin/foot "$X"/usr/bin/footclient)
# Sin los backends rdp/vnc/pipewire/x11/wayland/headless (arrastran muchas libs).
find "$X/$MULTI/libweston-13" "$X/$MULTI/weston" -name '*.so' 2>/dev/null |
    grep -E 'rdp|vnc|pipewire|x11|wayland-backend|headless|remoting' | xargs -r rm -f || true
plugs=$(find "$X/$MULTI/libweston-13" "$X/$MULTI/weston" -name '*.so' 2>/dev/null || true)
missing=$(for f in "${bins[@]}" $plugs "$X/$MULTI"/libweston-13.so.0 ; do
    [ -e "$f" ] && ldd "$f" 2>/dev/null | awk '/not found/ {print $1}'
done | sort -u || true)
if [ -n "$missing" ]; then
    echo "Faltan en el host/stage: $missing"
    echo "  (se intentan resolver desde el host con wayland-deps; instala los paquetes -dev/lib si no)"
fi

# Librerias del stage -> libs/glibc
find "$X/$MULTI" "$X/lib/x86_64-linux-gnu" -maxdepth 1 \( -type f -o -type l \) -name '*.so*' 2>/dev/null |
while read -r f; do cp -Lf "$f" "$LIBDIR/$(basename "$f")" 2>/dev/null || true; done || true

# Binarios
for b in $(cd "$X/usr/bin" && ls weston* wcap-decode foot footclient 2>/dev/null); do
    [ -f "$X/usr/bin/$b" ] && cp -f "$X/usr/bin/$b" "$PRE/usr/bin/$b" && echo "  bin: $b"
done

# Ayudantes de weston (desktop-shell cliente, teclado virtual...)
if [ -d "$X/usr/libexec" ]; then
    mkdir -p "$PRE/usr/libexec"; cp -a "$X/usr/libexec/." "$PRE/usr/libexec/"; echo "  dir: /usr/libexec"
fi

# Plugins, datos
for d in "$MULTI/libweston-13" "$MULTI/weston" "$MULTI/gbm" "usr/share/weston" \
         usr/share/fonts usr/share/glvnd usr/share/terminfo usr/share/fontconfig \
         usr/share/foot usr/share/X11; do
    if [ -d "$X/$d" ]; then
        mkdir -p "$PRE/$d"
        cp -a "$X/$d/." "$PRE/$d/"
        echo "  dir: /$d"
    fi
done
# libgbm/EGL cargan sus backends por ruta absoluta.
[ -d "$X/$MULTI/dri" ] && mkdir -p "$PRE/$MULTI/dri" && cp -a "$X/$MULTI/dri/." "$PRE/$MULTI/dri/" || true

# Dependencias que aun no esten (desde el host).
bash "$ROOT/scripts/wayland-deps.sh" --copy "${bins[@]}" $plugs \
    "$X/$MULTI/libweston-13.so.0" 2>/dev/null | grep -v '^have' || true
echo "OK"
