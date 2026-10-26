#!/bin/bash
# scripts/fetch-toolkits.sh — GTK3 y Qt5 de Ubuntu (sin compilar ni tocar) para
# probar que apps reales corren como ventanas de Aurora (via aurora-wayland).
# Requiere los paquetes instalados en el host de build (misma release que el
# sysroot, Ubuntu 24.04):
#   apt-get install gtk-3-examples librsvg2-common shared-mime-info \
#       adwaita-icon-theme hicolor-icon-theme gsettings-desktop-schemas \
#       qtbase5-examples qtwayland5 libqt5svg5 fonts-dejavu-core
# Copia binarios, plugins, datos y el cierre de librerias (ldd) que falte en
# libs/glibc/. Las libs del toolkit son las de la distro.
#   Apps:  gtk3-widget-factory gtk3-demo gtk3-icon-browser  (GTK3)
#          qt5-gallery qt5-application                      (Qt5 widgets)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBDIR="$ROOT/libs/glibc/lib/x86_64-linux-gnu"
PRE="$ROOT/precompiled"
MULTI=usr/lib/x86_64-linux-gnu
QT=/usr/lib/x86_64-linux-gnu/qt5
mkdir -p "$PRE/usr/bin" "$PRE/$MULTI" "$LIBDIR"

# --- binarios
for b in gtk3-widget-factory gtk3-demo gtk3-icon-browser gtk3-demo-application; do
    [ -x "/usr/bin/$b" ] && cp -f "/usr/bin/$b" "$PRE/usr/bin/$b" && echo "  bin: $b"
done
cp -f "$QT/examples/widgets/gallery/gallery" "$PRE/usr/bin/qt5-gallery"
cp -f "$QT/examples/widgets/mainwindows/application/application" "$PRE/usr/bin/qt5-application"

# --- plugins y datos
copy_dir() { # origen destino
    [ -d "$1" ] || return 0
    mkdir -p "$2"
    cp -a "$1/." "$2/"
    echo "  dir: ${1#/}"
}
G=/usr/lib/x86_64-linux-gnu
copy_dir $G/gdk-pixbuf-2.0 "$PRE/$MULTI/gdk-pixbuf-2.0"
copy_dir $G/gio/modules "$PRE/$MULTI/gio/modules"
for d in platforms wayland-decoration-client wayland-graphics-integration-client \
         wayland-shell-integration imageformats iconengines generic platforminputcontexts; do
    copy_dir "$QT/plugins/$d" "$PRE/$MULTI/qt5/plugins/$d"
done
# solo las plataformas Wayland (no xcb/eglfs...) para no arrastrar mas librerias
rm -f "$PRE/$MULTI/qt5/plugins/platforms/"lib{qxcb,qeglfs,qlinuxfb,qvnc,qminimal,qminimalegl,qoffscreen,qwayland-xcomposite-egl,qwayland-xcomposite-glx}.so
rm -rf "$PRE/$MULTI/qt5/plugins/wayland-graphics-integration-server" \
       "$PRE/$MULTI/qt5/plugins/platforminputcontexts"
copy_dir /usr/share/glib-2.0/schemas "$PRE/usr/share/glib-2.0/schemas"
# gschemas.compiled lo genera el postinst en el host
[ -f /usr/share/glib-2.0/schemas/gschemas.compiled ] || glib-compile-schemas "$PRE/usr/share/glib-2.0/schemas"
copy_dir /usr/share/mime "$PRE/usr/share/mime"
copy_dir /usr/share/icons/hicolor "$PRE/usr/share/icons/hicolor"
copy_dir /usr/share/icons/Adwaita "$PRE/usr/share/icons/Adwaita"
# el cursor lo dibuja Aurora: los temas de cursores sobran
rm -rf "$PRE/usr/share/icons/Adwaita/cursors"
copy_dir /usr/share/themes/Default "$PRE/usr/share/themes/Default"
copy_dir /usr/share/gtk-3.0 "$PRE/usr/share/gtk-3.0"
copy_dir /usr/share/glvnd "$PRE/usr/share/glvnd"
copy_dir /usr/share/locale/es "$PRE/usr/share/locale/es" || true
# cache de loaders de gdk-pixbuf (el postinst la genera en el host)
LC=$G/gdk-pixbuf-2.0/2.10.0/loaders.cache
[ -f "$LC" ] && mkdir -p "$PRE/$MULTI/gdk-pixbuf-2.0/2.10.0" && cp -f "$LC" "$PRE/$MULTI/gdk-pixbuf-2.0/2.10.0/loaders.cache"

# --- cierre de librerias
ELFS=$(find "$PRE/usr/bin/gtk3-"* "$PRE/usr/bin/qt5-"* \
    "$PRE/$MULTI/gdk-pixbuf-2.0" "$PRE/$MULTI/gio" "$PRE/$MULTI/qt5/plugins" \
    -type f \( -name '*.so*' -o -perm -u+x \) 2>/dev/null)
export LD_LIBRARY_PATH="$LIBDIR:$G"
copied=0
for pass in 1 2 3; do
    n=0
    for f in $ELFS; do
        file -b "$f" | grep -q ELF || continue
        ldd "$f" 2>/dev/null | awk '/=> \// {print $3}'
    done | sort -u | while read -r l; do
        b="$(basename "$l")"
        if [ ! -e "$LIBDIR/$b" ] && [ ! -L "$LIBDIR/$b" ]; then
            cp -Lf "$l" "$LIBDIR/$b" && echo "  lib: $b"
        fi
    done
    # tambien las librerias recien copiadas
    ELFS="$ELFS $(ls "$LIBDIR"/*.so* 2>/dev/null | head -1000)"
done
echo OK
