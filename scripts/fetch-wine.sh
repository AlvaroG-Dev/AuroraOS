#!/bin/bash
# scripts/fetch-wine.sh — Wine para AuroraOS (aplicaciones Windows como ventanas
# de Aurora). Baja Wine 11 de Arch Linux (WoW64 puro: un solo ntdll.so de 64 bits
# que ejecuta programas de 32 y 64 bits, sin librerias i386 de Linux) y las
# librerias opcionales que la ISO no trae (sonido ALSA/Pulse, GStreamer, xkbregistry
# para el driver Wayland...) de Ubuntu 24.04, la misma glibc 2.39 que la ISO.
#
# Deja el arbol en build/wine/root (usr/bin/wine, usr/lib/wine, usr/share/wine).
# El Makefile lo copia a la ISO con WITH_WINE=1 (por defecto). Necesita zstd en el
# host (apt install zstd) para abrir el paquete de Arch.
#
# Uso: scripts/fetch-wine.sh [--force]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/wine"
TREE="$OUT/root"
WINE_VER="${WINE_VER:-11.22-1}"
ARCH_MIRROR="${ARCH_MIRROR:-https://geo.mirror.pkgbuild.com}"
PKG="$OUT/arch/wine-$WINE_VER-x86_64.pkg.tar.zst"

[ "${1:-}" = "--force" ] && rm -rf "$TREE"
if [ -x "$TREE/usr/bin/wine" ] && [ -f "$TREE/.aurora-wine-$WINE_VER" ]; then
    echo "[wine] ya esta en $TREE"; exit 0
fi
command -v zstd >/dev/null || { echo "[wine] falta zstd en el host: apt install zstd" >&2; exit 1; }

mkdir -p "$OUT/arch" "$OUT/debs"
if [ ! -s "$PKG" ]; then
    echo "[wine] bajando wine $WINE_VER (Arch, ~80 MB)..."
    curl -fL --retry 4 -o "$PKG.part" "$ARCH_MIRROR/extra/os/x86_64/wine-$WINE_VER-x86_64.pkg.tar.zst"
    mv "$PKG.part" "$PKG"
fi
rm -rf "$TREE"; mkdir -p "$TREE"
tar --zstd -xf "$PKG" -C "$TREE" usr
rm -rf "$TREE/usr/share/man"
# Aspecto de Aurora en cada prefijo nuevo: wine.inf aplica installer/wine/
# aurora-wine.inf (paleta, Inter, sin msstyles) despues de ThemeManager.
INF="$TREE/usr/share/wine/wine.inf"
if ! grep -q '^\[AuroraTheme\]' "$INF"; then
    python3 -I - "$INF" "$ROOT/installer/wine/aurora-wine.inf" <<'PY'
import sys
inf, frag = sys.argv[1], sys.argv[2]
s = open(inf, encoding="utf-8", newline="").read()
a = s.index("[BaseInstall]")
b = s.index("ThemeManager", a)
s = s[:b] + "ThemeManager,\\\n    AuroraTheme" + s[b + len("ThemeManager"):]
s += open(frag, encoding="utf-8", newline="").read()
open(inf, "w", encoding="utf-8", newline="").write(s)
PY
fi

# Librerias que Wine carga (NEEDED o dlopen) y la ISO no trae. Ubuntu 24.04:
# misma glibc. Las que ya estan en libs/glibc se saltan.
PKGS=(libasound2t64 libasound2-data libpulse0 libxkbregistry0
      libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 gstreamer1.0-plugins-base
      libpcap0.8t64 libpcsclite1 libsane1 libgphoto2-6t64 libgphoto2-port12t64
      ocl-icd-libopencl1 libcups2t64 libsdl2-2.0-0 libunwind8 libgsm1 libosmesa6)
SKIP='^(libc6|libgcc-s1|libstdc\+\+6|bash|dash|coreutils|debconf|dpkg|perl-base|perl|base-files|gcc-14-base|gcc-13-base|debianutils|sed|grep|tar|gzip|findutils|diffutils|libcrypt1|libglib2\.0-0t64|libx11-6|libxcb1|libdbus-1-3|libsystemd0|libudev1|libwayland-.*|libdrm.*|libgl1.*|libegl.*|libgbm1|libvulkan1|mesa-.*|libllvm.*|adduser|init-system-helpers|systemd.*|libpam.*|lsb-base|sensible-utils|ucf|udev|libjpeg.*|libpng16-16t64|libtiff6|libfreetype6|libfontconfig1|fontconfig.*|libgnutls30t64|libkrb5-3|libgssapi-krb5-2|libxml2|libicu74|python3.*)$'
python3 -I "$ROOT/scripts/ubuntu-fetch-debs.py" "$OUT/debs" "$SKIP" "${PKGS[@]}"
EXTRA="$OUT/extra"
rm -rf "$EXTRA"; mkdir -p "$EXTRA"
for d in "$OUT"/debs/*.deb; do
    case "$(basename "$d")" in
        wine*|libwine*|fonts-wine*) continue ;;   # restos del Wine 9 de Ubuntu
    esac
    dpkg-deb -x "$d" "$EXTRA"
done
# Solo librerias (y datos de ALSA/GStreamer): nada de binarios ni doc.
GLIBC="$ROOT/libs/glibc/lib/x86_64-linux-gnu"
mkdir -p "$TREE/usr/lib/x86_64-linux-gnu"
for f in "$EXTRA"/usr/lib/x86_64-linux-gnu/*.so*; do
    b="$(basename "$f")"
    [ -e "$GLIBC/$b" ] && continue
    cp -a "$f" "$TREE/usr/lib/x86_64-linux-gnu/"
done
for d in gstreamer-1.0 alsa-lib sane; do
    [ -d "$EXTRA/usr/lib/x86_64-linux-gnu/$d" ] && cp -a "$EXTRA/usr/lib/x86_64-linux-gnu/$d" "$TREE/usr/lib/x86_64-linux-gnu/"
done
for d in alsa; do
    [ -d "$EXTRA/usr/share/$d" ] && { mkdir -p "$TREE/usr/share"; cp -a "$EXTRA/usr/share/$d" "$TREE/usr/share/"; }
done

# Comprobacion: todo NEEDED de Wine debe resolverse con libs/glibc + lo anterior.
export LD_LIBRARY_PATH="$TREE/usr/lib/x86_64-linux-gnu:$GLIBC"
miss=0
for f in "$TREE"/usr/lib/wine/x86_64-unix/*.so "$TREE"/usr/bin/wine "$TREE"/usr/bin/wineserver; do
    for n in $(readelf -d "$f" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        case "$n" in ntdll.so|win32u.so) continue ;; esac
        if [ ! -e "$TREE/usr/lib/x86_64-linux-gnu/$n" ] && [ ! -e "$GLIBC/$n" ]; then
            echo "  [wine] sin resolver: $n (de $(basename "$f"))"; miss=1
        fi
    done
done
[ $miss = 0 ] || echo "[wine] AVISO: faltan librerias opcionales (ese modulo de Wine no cargara)"
touch "$TREE/.aurora-wine-$WINE_VER"
echo "[wine] listo en $TREE ($(du -sh "$TREE" | cut -f1))"
