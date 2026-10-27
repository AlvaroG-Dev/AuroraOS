#!/bin/bash
# scripts/fetch-linux-drivers.sh — fuentes externas de la capa LinuxKPI
# (Plan_LinuxKPI.md, L0.2). Nada de esto se versiona: va a build/linuxkpi_src/
# (en .gitignore).
#
#   linux/              arbol de Linux (version fijada en
#                       kernel/linuxkpi/linux-version.mk); por defecto solo
#                       drivers/gpu, drivers/virtio (lo usa virtio-gpu), drivers/dma-buf (fences y
#                       dma_resv, L5), include,
#                       arch/x86/include, scripts y COPYING
#                       (--full para todo).
#   nvidia-open/        open-gpu-kernel-modules (kernel-open/ + src/).
#   linux-firmware/     linux-firmware (solo con --firmware; es grande).
#
# Uso:  scripts/fetch-linux-drivers.sh [--full] [--firmware] [--no-nvidia]
#       scripts/fetch-linux-drivers.sh --ubuntu
#
# --ubuntu: si kernel.org/GitHub no son accesibles, toma del archivo de Ubuntu
# (apt) linux-source y nvidia-kernel-source-*-open. ATENCION: no son las
# versiones fijadas (Ubuntu 24.04 trae Linux 6.8 con parches de Ubuntu); solo
# sirven para desarrollar y probar herramientas (linuxkpi-scan.py). Se dejan en
# ubuntu-linux/ y ubuntu-nvidia-open/ para que nunca se confundan con las
# fijadas.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
. "$ROOT/kernel/linuxkpi/linux-version.mk"
OUT="$ROOT/build/linuxkpi_src"
DL="$OUT/downloads"

FULL=0; FW=0; NV=1; UBUNTU=0
for a in "$@"; do
    case "$a" in
        --full) FULL=1 ;;
        --firmware) FW=1 ;;
        --no-nvidia) NV=0 ;;
        --ubuntu) UBUNTU=1 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "opcion desconocida: $a" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT" "$DL"

fetch() { # url fichero
    [ -s "$2" ] && return 0
    echo "  descargando $1"
    curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"
}

if [ "$UBUNTU" = 1 ]; then
    U="$DL/ubuntu"; mkdir -p "$U"
    cd "$U"
    ls linux-source-*.deb >/dev/null 2>&1 || apt-get download linux-source-6.8.0
    NVPKG=$(apt-cache search --names-only '^nvidia-kernel-source-[0-9]+-open$' |
            awk '{print $1}' | sort -V | tail -1)
    ls nvidia-kernel-source-*-open*.deb >/dev/null 2>&1 || apt-get download "$NVPKG"
    rm -rf "$OUT/ubuntu-linux" "$OUT/ubuntu-nvidia-open" x
    mkdir x
    for d in *.deb; do dpkg -x "$d" x; done
    TB=$(ls x/usr/src/linux-source-*/linux-source-*.tar.bz2 | head -1)
    mkdir -p "$OUT/ubuntu-linux"
    tar -xjf "$TB" -C "$OUT/ubuntu-linux" --strip-components=1
    NVD=$(ls -d x/usr/src/nvidia-* | head -1)
    cp -a "$NVD" "$OUT/ubuntu-nvidia-open"
    rm -rf x
    echo "$(basename "$TB" .tar.bz2) (Ubuntu, NO fijada)" > "$OUT/ubuntu-linux/.aurora-origin"
    echo "$(basename "$NVD") (Ubuntu, NO fijada)" > "$OUT/ubuntu-nvidia-open/.aurora-origin"
    echo "OK (provisional): $OUT/ubuntu-linux, $OUT/ubuntu-nvidia-open"
    exit 0
fi

# --- Linux ------------------------------------------------------------------
LTB="linux-$LINUX_VERSION.tar.xz"
fetch "https://cdn.kernel.org/pub/linux/kernel/v${LINUX_SERIES%%.*}.x/$LTB" "$DL/$LTB"
if [ ! -f "$OUT/linux/.aurora-version" ] ||
   [ "$(cat "$OUT/linux/.aurora-version")" != "$LINUX_VERSION full=$FULL virtio=1 dmabuf=1" ]; then
    rm -rf "$OUT/linux"; mkdir -p "$OUT/linux"
    if [ "$FULL" = 1 ]; then
        tar -xJf "$DL/$LTB" -C "$OUT/linux" --strip-components=1
    else
        P="linux-$LINUX_VERSION"
        tar -xJf "$DL/$LTB" -C "$OUT/linux" --strip-components=1 \
            "$P/COPYING" "$P/LICENSES" "$P/Makefile" "$P/drivers/gpu" "$P/drivers/virtio" "$P/drivers/dma-buf" \
            "$P/include" "$P/arch/x86/include" "$P/scripts"
    fi
    echo "$LINUX_VERSION full=$FULL virtio=1 dmabuf=1" > "$OUT/linux/.aurora-version"
fi
echo "OK: Linux $LINUX_VERSION en $OUT/linux"

# --- NVIDIA open-gpu-kernel-modules -----------------------------------------
if [ "$NV" = 1 ]; then
    NTB="open-gpu-kernel-modules-$NVIDIA_OPEN_VERSION.tar.gz"
    NGIT="https://github.com/NVIDIA/open-gpu-kernel-modules"
    if [ ! -f "$OUT/nvidia-open/.aurora-version" ] ||
       [ "$(cat "$OUT/nvidia-open/.aurora-version")" != "$NVIDIA_OPEN_VERSION" ]; then
        rm -rf "$OUT/nvidia-open"
        # El tarball de GitHub puede estar bloqueado donde git si funciona:
        # en ese caso, clon superficial de la misma etiqueta.
        if fetch "$NGIT/archive/refs/tags/$NVIDIA_OPEN_VERSION.tar.gz" "$DL/$NTB"; then
            mkdir -p "$OUT/nvidia-open"
            tar -xzf "$DL/$NTB" -C "$OUT/nvidia-open" --strip-components=1
        else
            rm -f "$DL/$NTB.part"
            echo "  tarball no disponible; clon git de la etiqueta $NVIDIA_OPEN_VERSION"
            git -c advice.detachedHead=false clone -q --depth 1 --branch "$NVIDIA_OPEN_VERSION" "$NGIT" "$OUT/nvidia-open"
            rm -rf "$OUT/nvidia-open/.git"
        fi
        echo "$NVIDIA_OPEN_VERSION" > "$OUT/nvidia-open/.aurora-version"
    fi
    echo "OK: NVIDIA open $NVIDIA_OPEN_VERSION en $OUT/nvidia-open"
fi

# --- linux-firmware ---------------------------------------------------------
if [ "$FW" = 1 ]; then
    if [ -z "$LINUX_FIRMWARE_TAG" ]; then
        echo "LINUX_FIRMWARE_TAG sin fijar en kernel/linuxkpi/linux-version.mk" >&2
        exit 1
    fi
    FTB="linux-firmware-$LINUX_FIRMWARE_TAG.tar.xz"
    fetch "https://cdn.kernel.org/pub/linux/kernel/firmware/$FTB" "$DL/$FTB"
    if [ ! -f "$OUT/linux-firmware/.aurora-version" ] ||
       [ "$(cat "$OUT/linux-firmware/.aurora-version")" != "$LINUX_FIRMWARE_TAG" ]; then
        rm -rf "$OUT/linux-firmware"; mkdir -p "$OUT/linux-firmware"
        tar -xJf "$DL/$FTB" -C "$OUT/linux-firmware" --strip-components=1
        echo "$LINUX_FIRMWARE_TAG" > "$OUT/linux-firmware/.aurora-version"
    fi
    echo "OK: linux-firmware $LINUX_FIRMWARE_TAG en $OUT/linux-firmware"
fi
