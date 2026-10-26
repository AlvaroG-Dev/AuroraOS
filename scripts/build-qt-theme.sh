#!/bin/bash
# Compila el plugin de tema de plataforma de Qt5 de Aurora (QT_QPA_PLATFORMTHEME=
# aurora) en el host contra la Qt de la distro (misma release que el sysroot) y
# lo deja en precompiled/. Necesita: qtbase5-dev qtbase5-private-dev.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/user/aurora-qt-theme"
OUT="$ROOT/precompiled/usr/lib/x86_64-linux-gnu/qt5/plugins/platformthemes"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
INC=/usr/include/x86_64-linux-gnu/qt5
QV=$(qmake -query QT_VERSION)
FLAGS="-I$INC -I$INC/QtCore -I$INC/QtGui -I$INC/QtWidgets \
  -I$INC/QtGui/$QV -I$INC/QtGui/$QV/QtGui -I$INC/QtCore/$QV -I$INC/QtCore/$QV/QtCore"
(cd "$SRC" && moc $FLAGS aurora-theme.cpp -o "$TMP/aurora-theme.moc")
g++ -std=c++17 -O2 -fPIC -shared -Wall -Wextra -DQT_PLUGIN -DQT_NO_DEBUG \
    $FLAGS -I"$SRC" -I"$TMP" -o "$OUT/libaurora.so" "$SRC/aurora-theme.cpp" \
    -lQt5Widgets -lQt5Gui -lQt5Core
echo "OK: $OUT/libaurora.so"
