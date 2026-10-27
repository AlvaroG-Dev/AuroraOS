#!/bin/bash
# scripts/install-linux-firmware.sh — copia a <destino>/lib/firmware los
# ficheros de linux-firmware listados en kernel/linuxkpi/firmware.list
# (Plan_LinuxKPI.md, L4). Resuelve los "Link:" de WHENCE y comprueba el
# SHA-256 de cada fichero. Sin el arbol descargado no hace nada.
#
# Uso: scripts/install-linux-firmware.sh <sysroot>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
. "$ROOT/kernel/linuxkpi/linux-version.mk"
DEST="${1:?uso: $0 <sysroot>}/lib/firmware"
SRC="$ROOT/build/linuxkpi_src/linux-firmware"
LIST="$ROOT/kernel/linuxkpi/firmware.list"

if [ ! -f "$SRC/WHENCE" ]; then
    echo "[firmware] sin $SRC (scripts/fetch-linux-drivers.sh --firmware): no se copia nada"
    exit 0
fi
if [ "$(cat "$SRC/.aurora-version" 2>/dev/null)" != "$LINUX_FIRMWARE_TAG" ]; then
    echo "[firmware] ERROR: $SRC no es la version $LINUX_FIRMWARE_TAG" >&2
    exit 1
fi

# Ruta real de un fichero: el propio, o el destino de su "Link:" (relativo al
# directorio del enlace), siguiendo enlaces encadenados.
resolve() {
    local p=$1 n=0 t
    while [ ! -f "$SRC/$p" ]; do
        t=$(awk -v l="$p" '$1 == "Link:" && $2 == l && $3 == "->" {print $4; exit}' "$SRC/WHENCE")
        [ -n "$t" ] || return 1
        case "$t" in /*) p=${t#/} ;; *) p="$(dirname "$p")/$t" ;; esac
        p=$(realpath -m --relative-to=/ "/$p")
        n=$((n + 1)); [ $n -lt 8 ] || return 1
    done
    echo "$p"
}

while read -r path sum _; do
    case "$path" in ''|'#'*) continue ;; esac
    real=$(resolve "$path") || { echo "[firmware] ERROR: $path no esta en linux-firmware" >&2; exit 1; }
    got=$(sha256sum "$SRC/$real" | cut -d' ' -f1)
    if [ "$got" != "$sum" ]; then
        echo "[firmware] ERROR: SHA-256 de $path ($real) no coincide" >&2
        exit 1
    fi
    mkdir -p "$DEST/$(dirname "$path")"
    cp -f "$SRC/$real" "$DEST/$path"
    echo "[firmware] $path${real:+ <- $real}"
done < "$LIST"
