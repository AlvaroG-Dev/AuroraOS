#!/bin/bash
# scripts/claude-env-setup.sh — prepara un contenedor de Claude Code (en la nube
# o local) para compilar y probar Aurora OS: paquetes del host, pila Wayland,
# fuentes de Linux fijadas y linux-firmware (LinuxKPI).
#
# Pensado para el "Setup script" del entorno de Claude Code en la web (se
# ejecuta como root antes de la sesion; puede que el repositorio aun no este
# clonado: entonces solo instala paquetes y el resto lo hace la primera
# ejecucion dentro del repositorio). Tambien vale a mano:
#     bash scripts/claude-env-setup.sh [ruta-del-repositorio]
#
# Idempotente: lo que ya esta no se repite. Variables:
#   AURORA_REPO=<ruta>      repositorio (si no, el argumento, $CLAUDE_PROJECT_DIR,
#                           el directorio actual o /home/user/AuroraOS)
#   AURORA_NO_LINUX=1       no descargar Linux 6.18.59 (~150 MiB)
#   AURORA_NO_FIRMWARE=1    no descargar linux-firmware 20261019 (~641 MiB,
#                           2 GiB extraido; sin el, se omite la prueba del
#                           firmware real de LinuxKPI)
#   AURORA_NO_WAYLAND=1     no preparar la pila Wayland (scripts/setup-env.sh);
#                           sin ella `make iso` no construye el userland grafico
set -uo pipefail

say()  { echo "[claude-env] $*"; }
warn() { echo "[claude-env] AVISO: $*" >&2; }

SUDO=""
[ "$(id -u)" != "0" ] && command -v sudo >/dev/null && SUDO="sudo"
export DEBIAN_FRONTEND=noninteractive

# --- 1. Paquetes del host ----------------------------------------------------
# Lo mismo que scripts/setup-env.sh (compilacion, bootloader, ISO, QEMU/OVMF,
# iconos, aurora-wayland) y lo de las pruebas (musl-gcc, rsync, xclip...).
PKGS=(build-essential binutils nasm clang lld git curl file xxd
      qemu-system-x86 ovmf mtools dosfstools xorriso gdisk
      python3 python3-pil librsvg2-bin fonts-dejavu-core dpkg-dev
      libpng-dev libxcb1-dev libxcb-composite0-dev libxcb-xfixes0-dev
      rsync musl-tools xz-utils ca-certificates xclip xsel imagemagick)
missing=()
for p in "${PKGS[@]}"; do
    dpkg-query -W -f='${db:Status-Abbrev}' "$p" 2>/dev/null | grep -q '^ii' || missing+=("$p")
done
if [ ${#missing[@]} -gt 0 ]; then
    say "instalando: ${missing[*]}"
    # musl-tools a veces no aparece sin un apt-get update previo.
    $SUDO apt-get update -qq || warn "apt-get update fallo"
    $SUDO apt-get install -y -qq "${missing[@]}" || {
        warn "fallo la instalacion conjunta; se prueba uno a uno"
        for p in "${missing[@]}"; do
            $SUDO apt-get install -y -qq "$p" || warn "no se pudo instalar $p"
        done
    }
fi
command -v musl-gcc >/dev/null || warn "falta musl-gcc (pruebas de user/musl)"

# --- 2. Repositorio ----------------------------------------------------------
REPO="${AURORA_REPO:-${1:-${CLAUDE_PROJECT_DIR:-}}}"
if [ -z "$REPO" ] || [ ! -f "$REPO/Plan_LinuxKPI.md" ]; then
    for c in "$PWD" /home/user/AuroraOS /workspace/AuroraOS "$HOME/AuroraOS"; do
        if [ -f "$c/Plan_LinuxKPI.md" ]; then REPO=$c; break; fi
    done
fi
if [ -z "$REPO" ] || [ ! -f "$REPO/Plan_LinuxKPI.md" ]; then
    say "repositorio no encontrado (aun sin clonar?): solo paquetes instalados."
    say "dentro del repositorio: bash scripts/claude-env-setup.sh"
    exit 0
fi
cd "$REPO" || exit 1
say "repositorio: $REPO"

# --- 3. Pila Wayland, iconos, discos de prueba, OVMF_VARS ---------------------
if [ "${AURORA_NO_WAYLAND:-0}" != "1" ]; then
    bash scripts/setup-env.sh || warn "scripts/setup-env.sh termino con error"
fi

# --- 4. Fuentes de Linux y linux-firmware (LinuxKPI) --------------------------
ARGS=(--no-nvidia)
[ "${AURORA_NO_FIRMWARE:-0}" != "1" ] && ARGS+=(--firmware)
if [ "${AURORA_NO_LINUX:-0}" != "1" ]; then
    say "fuentes de Linux fijadas ${ARGS[*]} (build/linuxkpi_src, no se versiona)"
    bash scripts/fetch-linux-drivers.sh "${ARGS[@]}" || warn "fetch-linux-drivers.sh fallo"
fi

# --- 5. Comprobacion ---------------------------------------------------------
for t in musl-gcc qemu-system-x86_64 xorriso nasm rsync; do
    command -v "$t" >/dev/null || warn "falta $t"
done
[ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] || warn "falta OVMF (paquete ovmf)"
say "listo. ISO: make iso MUSL_CC=/usr/bin/musl-gcc KUR_CC=gcc"
say "pruebas: python3 scripts/qemu-drive.py -m 6G --done 'passed, '   (suite del kernel)"
say "         python3 scripts/qemu-drive.py -t 1200 -m 6G --boot-test /usr/bin/lkpi_test --done 'LKPI-TEST:'"
