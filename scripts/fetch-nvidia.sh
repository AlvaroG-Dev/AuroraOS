#!/bin/bash
# scripts/fetch-nvidia.sh — binarios de NVIDIA (Plan_NVIDIA.md, T0.3).
# Nada de esto se versiona: va a build/nvidia/ (dentro de build/, ignorado).
#
#   build/nvidia/NVIDIA-Linux-x86_64-<ver>.run   paquete oficial (descarga)
#   build/nvidia/pkg/                            contenido del paquete
#   build/nvidia/firmware/                       gsp_*.bin (GSP-RM) del paquete
#   build/nvidia/userlib/                        libnvidia-*, libEGL_nvidia, ...
#
# El .run NUNCA se ejecuta: es un script makeself (cabecera de shell + tar zstd/xz).
# Se lee la cabecera como texto, se localiza el inicio de la carga (`skip=`) y
# se extrae solo el tar con `tail | zstd | tar`, que es equivalente a -x pero sin
# ejecutar codigo de la descarga.
#
# Las fuentes del nucleo abierto (open-gpu-kernel-modules) las baja
# scripts/fetch-linux-drivers.sh a build/linuxkpi_src/nvidia-open; este script
# solo comprueba que esten y que su version coincida con la del paquete.
# La version sale de kernel/linuxkpi/linux-version.mk (NVIDIA_OPEN_VERSION):
# firmware GSP, userspace y modulos deben ser SIEMPRE la misma.
#
# Uso: scripts/fetch-nvidia.sh [--no-extract]
#      NVIDIA_RUN_URL=<url> para un espejo distinto;
#      NVIDIA_RUN_SHA256=<hash> para exigir una suma concreta.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
. "$ROOT/kernel/linuxkpi/linux-version.mk"
V="$NVIDIA_OPEN_VERSION"
OUT="$ROOT/build/nvidia"
RUN="NVIDIA-Linux-x86_64-$V.run"
URL="${NVIDIA_RUN_URL:-https://download.nvidia.com/XFree86/Linux-x86_64/$V/$RUN}"
EXTRACT=1
for a in "$@"; do
    case "$a" in
        --no-extract) EXTRACT=0 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "opcion desconocida: $a" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT"

# El .run de 615.x lleva la carga en zstd: se comprueba antes de bajar 500 MB.
if [ "$EXTRACT" = 1 ] && ! command -v zstd >/dev/null 2>&1 && ! command -v unzstd >/dev/null 2>&1; then
    echo "falta zstd (necesario para extraer el firmware): sudo apt install zstd" >&2
    exit 1
fi

SRC="$ROOT/build/linuxkpi_src/nvidia-open"
if [ "$(cat "$SRC/.aurora-version" 2>/dev/null)" != "$V" ]; then
    echo "faltan las fuentes de NVIDIA open $V: ejecuta scripts/fetch-linux-drivers.sh" >&2
    exit 1
fi
echo "OK: fuentes NVIDIA open $V"

if [ ! -s "$OUT/$RUN" ]; then
    echo "  descargando $URL"
    curl -fL --retry 3 -C - -o "$OUT/$RUN.part" "$URL" && mv "$OUT/$RUN.part" "$OUT/$RUN"
fi
SUM="$(sha256sum "$OUT/$RUN" | cut -d' ' -f1)"
if [ -n "${NVIDIA_RUN_SHA256:-}" ] && [ "$SUM" != "$NVIDIA_RUN_SHA256" ]; then
    echo "sha256 inesperado: $SUM" >&2; exit 1
fi
echo "OK: $RUN ($(du -h "$OUT/$RUN" | cut -f1)) sha256=$SUM"
[ "$EXTRACT" = 1 ] || exit 0

if [ ! -f "$OUT/pkg/.aurora-version" ] || [ "$(cat "$OUT/pkg/.aurora-version")" != "$V" ]; then
    rm -rf "$OUT/pkg"; mkdir -p "$OUT/pkg"
    # makeself: la cabecera tiene `skip=N` (linea donde empieza la carga).
    SKIP="$(head -n 700 "$OUT/$RUN" | sed -n 's/^skip="\{0,1\}\([0-9]\+\)"\{0,1\}$/\1/p' | head -1 || true)"
    [ -n "$SKIP" ] || { echo "no se encuentra skip= en la cabecera de $RUN" >&2; exit 1; }
    # La carga es zstd (28 b5 2f fd) en 615.x; xz (fd 37 7a 58) en versiones antiguas.
    MAGIC="$(tail -n +"$SKIP" "$OUT/$RUN" | head -c 4 | od -An -tx1 | tr -d ' \n' || true)"
    case "$MAGIC" in
        28b52ffd) if command -v zstd >/dev/null 2>&1; then DEC="zstd -dc"; else DEC="unzstd -c"; fi ;;
        fd377a58) DEC="xz -dc" ;;
        *) echo "formato de carga desconocido ($MAGIC)" >&2; exit 1 ;;
    esac
    tail -n +"$SKIP" "$OUT/$RUN" | $DEC | tar -x -C "$OUT/pkg"
    echo "$V" > "$OUT/pkg/.aurora-version"
fi
rm -rf "$OUT/firmware" "$OUT/userlib"
mkdir -p "$OUT/firmware" "$OUT/userlib"
cp -a "$OUT"/pkg/firmware/. "$OUT/firmware/"
# Componentes de usuario (equivale a --no-kernel-modules): solo bibliotecas.
find "$OUT/pkg" -maxdepth 1 -name '*.so*' -exec cp -a {} "$OUT/userlib/" \;
echo "OK: firmware GSP:"; ls -l "$OUT/firmware"
echo "OK: $(ls "$OUT/userlib" | wc -l) ficheros de biblioteca en $OUT/userlib"
