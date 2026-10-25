#!/bin/bash
# Fuentes, fontconfig y locale C.UTF-8 para las apps Wayland (foot, GTK...).
# /etc es un tmpfs en Aurora, asi que el fonts.conf vive en /usr/share/fontconfig
# y se usa con FONTCONFIG_FILE (ver user/musl/apps/init).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEBS="$ROOT/build/wayland_stage/debs"
TMP="$ROOT/build/wayland_stage/fonts"
DST="$ROOT/precompiled/usr/share/fonts/truetype/dejavu"
mkdir -p "$DEBS" "$TMP" "$DST" "$ROOT/precompiled/usr/share/fontconfig"
cd "$DEBS"
ls fonts-dejavu-mono_*.deb >/dev/null 2>&1 || apt-get download fonts-dejavu-mono >/dev/null
dpkg -x fonts-dejavu-mono_*.deb "$TMP"
cp -f "$TMP"/usr/share/fonts/truetype/dejavu/*.ttf "$DST/"
cat > "$ROOT/precompiled/usr/share/fontconfig/fonts.conf" <<'XML'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>/usr/share/fonts</dir>
  <cachedir>/tmp/fontconfig-cache</cachedir>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias>
</fontconfig>
XML
echo "OK: fuentes DejaVu (mono incl.) + fonts.conf"

# Locale C.UTF-8 (foot y otras apps exigen un locale UTF-8; en Ubuntu son ficheros
# de libc-bin bajo /usr/lib/locale/C.utf8).
cd "$DEBS"
ls libc-bin_*.deb >/dev/null 2>&1 || apt-get download libc-bin >/dev/null
rm -rf "$TMP/libc-bin" && mkdir -p "$TMP/libc-bin"
dpkg -x libc-bin_*.deb "$TMP/libc-bin"
mkdir -p "$ROOT/precompiled/usr/lib/locale"
rm -rf "$ROOT/precompiled/usr/lib/locale/C.utf8"
cp -r "$TMP/libc-bin/usr/lib/locale/C.utf8" "$ROOT/precompiled/usr/lib/locale/C.utf8"
echo "OK: locale C.utf8"
