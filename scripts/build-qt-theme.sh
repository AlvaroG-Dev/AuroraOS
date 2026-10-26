#!/bin/bash
# Compila los plugins de Qt5 de Aurora: el tema de plataforma (QT_QPA_PLATFORMTHEME=
# aurora) y el estilo "aurora" de widgets (QT_STYLE_OVERRIDE / StyleNames) en el host contra la Qt de la distro (misma release que el sysroot) y
# lo deja en precompiled/. Necesita: qtbase5-dev qtbase5-private-dev.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/user/aurora-qt-theme"
PLUG="$ROOT/precompiled/usr/lib/x86_64-linux-gnu/qt5/plugins"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$PLUG/platformthemes" "$PLUG/styles"
INC=/usr/include/x86_64-linux-gnu/qt5
QV=$(qmake -query QT_VERSION)
FLAGS="-I$INC -I$INC/QtCore -I$INC/QtGui -I$INC/QtWidgets \
  -I$INC/QtGui/$QV -I$INC/QtGui/$QV/QtGui -I$INC/QtCore/$QV -I$INC/QtCore/$QV/QtCore"
build() { # fuente salida
    (cd "$SRC" && moc $FLAGS "$1.cpp" -o "$TMP/$1.moc")
    g++ -std=c++17 -O2 -fPIC -shared -Wall -Wextra -DQT_PLUGIN -DQT_NO_DEBUG \
        $FLAGS -I"$SRC" -I"$TMP" -o "$2" "$SRC/$1.cpp" \
        -lQt5Widgets -lQt5Gui -lQt5Core
    echo "OK: $2"
}
build aurora-theme "$PLUG/platformthemes/libaurora.so"
build aurora-style "$PLUG/styles/libauroraqstyle.so"
