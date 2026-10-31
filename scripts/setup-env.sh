#!/bin/bash
# scripts/setup-env.sh — Prepara el entorno de build de Aurora OS si no lo esta ya.
#
# Idempotente y rapido cuando todo esta en su sitio: solo comprueba. Lo invoca
# el Makefile (target `env`) antes de construir la ISO; tambien se puede lanzar
# a mano. Cada paso solo actua si le falta algo:
#
#   1. paquetes del host (apt)            -> sudo apt-get install
#   2. toolchain musl (toolchain/*.tgz)   -> se descomprime
#   3. cabeceras del kernel               -> una vez, limpia objetos sin .d
#   4. imagenes de disco de prueba        -> tests/test_disk.img, extra_disk.img
#   5. iconos del Dock y del sistema      -> assets/system/icons/*.bmp
#   6. pila Wayland (libs, weston, foot, fuentes, aurora-wayland, GTK/Qt, X11)
#   7. OVMF_VARS.fd
#   8. LinuxKPI: arbol de Linux fijado + linux-firmware (fetch-linux-drivers.sh)
#
# Variables:
#   WITH_MESA=1   tambien baja Mesa/LLVM (scripts/fetch-mesa.sh; +500 MB en el
#                 initrd, necesita QEMU con >= 1,5 GB)
#   WITH_LKPI=0   no baja el arbol de Linux ni linux-firmware (por defecto 1: la
#                 capa LinuxKPI y sus drivers no compilan sin ellos)
#   WITH_NVIDIA=0 no baja open-gpu-kernel-modules ni compila nvidia*.kur (por
#                 defecto 1: el Makefile construye el driver NVIDIA siempre)
#   SKIP_ENV=1    no hace nada (lo respeta el Makefile)
#   ENV_OFFLINE=1 no intenta instalar ni descargar: solo avisa de lo que falta
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

[ "${SKIP_ENV:-0}" = "1" ] && exit 0

say()  { echo "[env] $*"; }
warn() { echo "[env] AVISO: $*" >&2; }
die()  { echo "[env] ERROR: $*" >&2; exit 1; }

OFFLINE=${ENV_OFFLINE:-0}
CHANGED=0

# Ejecuta un paso de preparacion; si falla, corta con un mensaje claro.
run_step() {
    local what=$1; shift
    say "$what"
    CHANGED=1
    if [ "$OFFLINE" = "1" ]; then warn "ENV_OFFLINE=1: no se ejecuta '$*'"; return 1; fi
    "$@" || die "fallo en: $what ($*)"
}

# Igual, pero un fallo solo avisa (componentes opcionales: GTK/Qt, Xwayland).
soft_step() {
    local what=$1; shift
    say "$what"
    CHANGED=1
    if [ "$OFFLINE" = "1" ]; then warn "ENV_OFFLINE=1: no se ejecuta '$*'"; return 0; fi
    "$@" || warn "fallo (opcional) en: $what ($*)"
}

# --- 1. Paquetes del host -----------------------------------------------------
# Compilacion, bootloader (clang + lld-link), ISO, QEMU/OVMF, iconos y descarga
# de los .deb de la pila Wayland (apt-get download no necesita root).
PKGS=(build-essential binutils nasm clang lld git curl file xxd
      qemu-system-x86 ovmf mtools dosfstools xorriso gdisk
      python3 python3-pil librsvg2-bin fonts-dejavu-core dpkg-dev
      rsync musl-tools patch                                   # python-stdlib; musl-gcc de las pruebas
      libpng-dev libxcb1-dev libxcb-composite0-dev libxcb-xfixes0-dev)  # aurora-wayland

missing=()
for p in "${PKGS[@]}"; do
    dpkg-query -W -f='${db:Status-Abbrev}' "$p" 2>/dev/null | grep -q '^ii' || missing+=("$p")
done
if [ ${#missing[@]} -gt 0 ]; then
    say "faltan paquetes del host: ${missing[*]}"
    CHANGED=1
    if [ "$OFFLINE" = "1" ]; then
        warn "ENV_OFFLINE=1: instala a mano: sudo apt-get install ${missing[*]}"
    else
        SUDO=""
        [ "$(id -u)" != "0" ] && SUDO="sudo"
        $SUDO apt-get update -qq && $SUDO apt-get install -y "${missing[@]}" \
            || die "no se pudieron instalar: ${missing[*]}  (sudo apt-get install ${missing[*]})"
    fi
fi

# --- 2. Toolchain musl --------------------------------------------------------
MUSL_DIR=toolchain/x86_64-linux-musl-cross
if [ ! -x "$MUSL_DIR/bin/x86_64-linux-musl-gcc" ]; then
    if [ -f "$MUSL_DIR.tgz" ]; then
        run_step "descomprimiendo $MUSL_DIR.tgz" tar -xzf "$MUSL_DIR.tgz" -C toolchain
    else
        warn "falta $MUSL_DIR y no hay $MUSL_DIR.tgz: user/musl no compilara (init, apps...)"
    fi
fi

# --- 3. Dependencias de cabeceras del kernel ----------------------------------
# kernel/Makefile genera ahora un .d por objeto (-MMD). Los .o anteriores a eso
# no tienen .d y pueden estar compilados con structs viejas: se tiran una vez.
if [ ! -f kernel/.deps-stamp ]; then
    if [ -n "$(find kernel -name '*.o' -not -name '*blob*' -print -quit 2>/dev/null)" ]; then
        say "kernel: objetos sin dependencias de cabeceras, se recompilan una vez"
        find kernel -name '*.o' -not -name '*blob*' -delete
        CHANGED=1
    fi
    touch kernel/.deps-stamp
fi

# --- 4. Imagenes de disco de prueba -------------------------------------------
if [ ! -f tests/test_disk.img ]; then
    run_step "creando tests/test_disk.img" bash scripts/mk_test_disk.sh >/dev/null
fi
if [ ! -f tests/extra_disk.img ]; then
    run_step "creando tests/extra_disk.img" truncate -s 64M tests/extra_disk.img
fi

# --- 5. Iconos ----------------------------------------------------------------
# Dock: app-*.bmp (generados). Sistema: *.bmp a partir de kernel/pre-icons.
if ! ls assets/system/icons/app-*.bmp >/dev/null 2>&1; then
    run_step "generando iconos del Dock (gen-dock-icons.py)" python3 scripts/gen-dock-icons.py
fi
if [ ! -f assets/system/icons/folder-icon.bmp ] && [ -d kernel/pre-icons ]; then
    run_step "convirtiendo kernel/pre-icons a assets/system/icons" bash scripts/tools/build_icons.sh
fi

# --- 6. Pila Wayland -----------------------------------------------------------
LIBDIR=libs/glibc/lib/x86_64-linux-gnu
PRE=precompiled/usr

# libwayland, libdrm, xkbcommon, libinput...
if [ ! -e "$LIBDIR/libwayland-server.so.0" ] || [ ! -e "$LIBDIR/libxkbcommon.so.0" ]; then
    run_step "pila Wayland (fetch-wayland.sh)" bash scripts/fetch-wayland.sh
fi
# weston + foot + clientes de prueba
if [ ! -x "$PRE/bin/weston" ] || [ ! -x "$PRE/bin/foot" ]; then
    run_step "weston/foot (fetch-weston.sh)" bash scripts/fetch-weston.sh
fi
# Inter / JetBrains Mono / DejaVu + fontconfig
if [ ! -f "$PRE/share/fontconfig/fonts.conf" ] || [ ! -d "$PRE/share/fonts/opentype/inter" ]; then
    run_step "fuentes (fetch-fonts.sh)" bash scripts/fetch-fonts.sh
fi
# aurora-wayland: se recompila si falta o si cambian sus fuentes
AW_BIN="$PRE/bin/aurora-wayland"
if [ ! -x "$AW_BIN" ] || [ -n "$(find user/aurora-wayland -type f \( -name '*.c' -o -name '*.h' -o -name '*.xml' \) -newer "$AW_BIN" -print -quit 2>/dev/null)" ]; then
    run_step "aurora-wayland (build-aurora-wayland.sh)" bash scripts/build-aurora-wayland.sh
fi
# GTK3 / Qt5 (apps de prueba). Requieren paquetes del host.
if [ ! -x "$PRE/bin/gtk3-widget-factory" ] || [ ! -x "$PRE/bin/qt5-gallery" ]; then
    tk=(gtk-3-examples librsvg2-common shared-mime-info adwaita-icon-theme
        hicolor-icon-theme gsettings-desktop-schemas qtbase5-examples qtwayland5
        libqt5svg5 fonts-dejavu-core)
    tkmiss=()
    for p in "${tk[@]}"; do dpkg-query -W -f='${db:Status-Abbrev}' "$p" 2>/dev/null | grep -q '^ii' || tkmiss+=("$p"); done
    if [ ${#tkmiss[@]} -gt 0 ] && [ "$OFFLINE" != "1" ]; then
        say "paquetes de toolkits: ${tkmiss[*]}"
        SUDO=""; [ "$(id -u)" != "0" ] && SUDO="sudo"
        $SUDO apt-get install -y "${tkmiss[@]}" || warn "no se instalaron: ${tkmiss[*]}"
    fi
    soft_step "GTK3/Qt5 (fetch-toolkits.sh)" bash scripts/fetch-toolkits.sh
fi
# Modulo GTK3 y plugins Qt5 de Aurora: se recompilan si faltan o cambian sus fuentes
GTKM="$PRE/lib/aurora-gtk/modules/libaurora-theme.so"
if [ ! -f "$GTKM" ] || [ -n "$(find user/aurora-gtk-module -type f -newer "$GTKM" -print -quit 2>/dev/null)" ]; then
    soft_step "modulo GTK3 de Aurora (build-gtk-module.sh)" bash scripts/build-gtk-module.sh
fi
QTP="$PRE/lib/x86_64-linux-gnu/qt5/plugins/platformthemes/libaurora.so"
if [ ! -f "$QTP" ] || [ -n "$(find user/aurora-qt-theme -type f -newer "$QTP" -print -quit 2>/dev/null)" ]; then
    soft_step "plugins Qt5 de Aurora (build-qt-theme.sh)" bash scripts/build-qt-theme.sh
fi
# Xwayland + apps X11
if [ ! -x "$PRE/bin/Xwayland" ]; then
    xw=(xwayland x11-apps xterm x11-xkb-utils xkb-data xclip xsel libxcb1-dev libxcb-composite0-dev)
    xwmiss=()
    for p in "${xw[@]}"; do dpkg-query -W -f='${db:Status-Abbrev}' "$p" 2>/dev/null | grep -q '^ii' || xwmiss+=("$p"); done
    if [ ${#xwmiss[@]} -gt 0 ] && [ "$OFFLINE" != "1" ]; then
        SUDO=""; [ "$(id -u)" != "0" ] && SUDO="sudo"
        $SUDO apt-get install -y "${xwmiss[@]}" || warn "no se instalaron: ${xwmiss[*]}"
    fi
    soft_step "Xwayland (fetch-xwayland.sh)" bash scripts/fetch-xwayland.sh
fi
# Mesa/LLVM: opcional por tamano
if [ "${WITH_MESA:-0}" = "1" ] && ! ls "$LIBDIR"/libgallium* >/dev/null 2>&1; then
    run_step "Mesa (fetch-mesa.sh)" bash scripts/fetch-mesa.sh
fi

# --- 7. OVMF_VARS.fd -----------------------------------------------------------
if [ ! -f OVMF_VARS.fd ] && [ -f /usr/share/OVMF/OVMF_VARS_4M.fd ]; then
    say "copiando OVMF_VARS.fd"
    cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
fi

# --- 8. LinuxKPI: fuentes de Linux y firmware ------------------------------------
# build/linuxkpi_src/linux (version fijada en kernel/linuxkpi/linux-version.mk) y
# build/linuxkpi_src/linux-firmware. No se versionan. Sin ellos `make iso` falla
# al compilar linuxkpi y sus .kur (i915, drm, ttm...).
if [ "${WITH_LKPI:-1}" = "1" ]; then
    lk_args=(--firmware)
    [ "${WITH_NVIDIA:-1}" = "1" ] || lk_args+=(--no-nvidia)
    if [ ! -f build/linuxkpi_src/linux/drivers/gpu/drm/i915/i915_driver.c ] || \
       [ ! -d build/linuxkpi_src/linux-firmware ]; then
        run_step "fuentes de Linux y linux-firmware (fetch-linux-drivers.sh ${lk_args[*]})" \
            bash scripts/fetch-linux-drivers.sh "${lk_args[@]}"
    fi
fi

# --- Avisos de cosas que no se pueden regenerar solas --------------------------
[ -x build/tcc_stage/usr/bin/tcc ]    || warn "falta build/tcc_stage (tcc+musl+ncurses): 'git checkout build/tcc_stage'"
[ -f third_party/zork1.z3 ]           || warn "falta third_party/zork1.z3 (Zork no estara en la ISO)"
[ -x third_party/frotz/frotz ]        || warn "falta third_party/frotz/frotz (frotz no estara en la ISO)"

[ "$CHANGED" = "1" ] && say "entorno preparado" || say "entorno OK"
exit 0
