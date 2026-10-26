#!/bin/bash
# scripts/fetch-mesa.sh — Mesa de Ubuntu (sin compilar ni tocar) para clientes
# OpenGL/EGL por software (llvmpipe/softpipe) dentro de AuroraOS:
#   libgallium (DRI swrast/kms_swrast), libLLVM, libEGL/libGLESv2/libgbm y las
#   librerias que arrastran. Las libs van a libs/glibc/, los drivers DRI a
#   precompiled/usr/lib/x86_64-linux-gnu/dri.
# Los clientes Wayland usan la plataforma EGL "wayland" con wl_shm (swrast): no
# hace falta DRM ni dma-buf.
#
# Uso:  scripts/fetch-mesa.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/build/mesa_stage"
DEBS="$STAGE/debs"
X="$STAGE/root"
LIBDIR="$ROOT/libs/glibc/lib/x86_64-linux-gnu"
PRE="$ROOT/precompiled"
MULTI=usr/lib/x86_64-linux-gnu

PKGS=(
    mesa-libgallium libgl1-mesa-dri libegl-mesa0 libgbm1 libglapi-mesa
    libegl1 libgles2 libglvnd0 libgl1 libglx-mesa0 libglx0
    libllvm20 libdrm-amdgpu1 libdrm-intel1 libdrm-radeon1 libdrm-nouveau2
    libpciaccess0 libsensors5 libelf1t64 libx11-xcb1 libxcb-dri3-0
    libxcb-present0 libxcb-randr0 libxcb-sync1 libxcb-xfixes0 libxcb-shm0
    libxcb-dri2-0 libxcb-glx0 libxshmfence1 libexpat1 libzstd1 libstdc++6
    libxxf86vm1 libxext6 libxfixes3 libx11-6 libxcb1 libxau6 libxdmcp6
    libedit2 libbsd0 libmd0 libtinfo6 libxml2 libicu74 liblzma5
    "$@"
)
mkdir -p "$DEBS" "$X" "$LIBDIR" "$PRE/$MULTI"
cd "$DEBS"
for p in "${PKGS[@]}"; do
    ls "${p}"_*.deb >/dev/null 2>&1 || apt-get download "$p" >/dev/null 2>&1 \
        || echo "  (sin paquete: $p)"
done
for d in *.deb; do dpkg -x "$d" "$X"; done

# Librerias -> libs/glibc (no se pisan las que ya estan: son de la misma distro).
find "$X/$MULTI" -maxdepth 1 \( -type f -o -type l \) -name '*.so*' |
while read -r f; do
    b="$(basename "$f")"
    [ -e "$LIBDIR/$b" ] || [ -L "$LIBDIR/$b" ] || cp -a "$f" "$LIBDIR/$b"
done
# libgallium (el DRI de Mesa 25 vive en una libreria propia) y los .so DRI.
mkdir -p "$PRE/$MULTI/dri"
cp -af "$X/$MULTI"/libgallium-*.so "$LIBDIR/" 2>/dev/null || true
[ -d "$X/$MULTI/dri" ] && cp -a "$X/$MULTI/dri/." "$PRE/$MULTI/dri/" || true
# EGL/vendor de glvnd y plugins de GBM.
for d in usr/share/glvnd usr/share/drirc.d "$MULTI/gbm"; do
    [ -d "$X/$d" ] && mkdir -p "$PRE/$d" && cp -a "$X/$d/." "$PRE/$d/" || true
done
echo "OK: Mesa en libs/glibc y precompiled/$MULTI/dri"
ls -la "$LIBDIR"/libgallium* "$LIBDIR"/libLLVM* 2>/dev/null || true
