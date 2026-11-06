#!/bin/bash
# Genera el initrd MINIMO del sistema instalado (va en la ESP del disco):
#   /usr/bin/init = aurora-boot (monta root=, pivot_root, exec del init real),
#   /etc (semillas del tmpfs de /etc, las lee el kernel antes del pivot),
#   drivers .aur/.kur y firmware que el kernel necesita ANTES de montar la raiz,
#   /system (config, iconos, fondo por defecto).
# Uso: scripts/mk-boot-initrd.sh <sysroot> <salida.tar>
set -e
SYS=${1:?sysroot}; OUT=${2:?salida}
BOOT=$(dirname "$0")/../user/musl/aurora-boot
[ -x "$BOOT" ] || { echo "falta $BOOT"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/usr/bin" "$T/usr/lib" "$T/lib" "$T/system"
mkdir -p "$T"/{proc,dev,sys,run,tmp,newroot,oldroot,data,root,bin,sbin}
cp "$BOOT" "$T/usr/bin/init"
# Los drivers .aur (ahci, xhci, ps2...) son ELF dinamicos: necesitan ld.so + libc.
mkdir -p "$T/lib64" "$T/lib/x86_64-linux-gnu" "$T/usr/lib/x86_64-linux-gnu"
cp -aL "$SYS/lib64/ld-linux-x86-64.so.2" "$T/lib64/"
for l in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libgcc_s.so.1 libtinfo.so.6; do
  for d in lib/x86_64-linux-gnu usr/lib/x86_64-linux-gnu; do
    [ -e "$SYS/$d/$l" ] && cp -aL "$SYS/$d/$l" "$T/$d/"
  done
done
[ -d "$SYS/etc" ] && cp -a "$SYS/etc" "$T/"
[ -d "$SYS/usr/lib/aur" ] && cp -a "$SYS/usr/lib/aur" "$T/usr/lib/"
for d in modules firmware; do [ -d "$SYS/lib/$d" ] && cp -a "$SYS/lib/$d" "$T/lib/"; done
# /system sin el catalogo completo de fondos (solo el de por defecto).
(cd "$SYS" && tar --exclude='system/wallpapers/*' -cf - system) | tar -xf - -C "$T"
mkdir -p "$T/system/wallpapers"
cp -a "$SYS"/system/wallpapers/aurora-noche.bmp "$SYS"/system/wallpapers/*claro* "$T/system/wallpapers/" 2>/dev/null || true
tar --format=ustar --owner=0 --group=0 -cf "$OUT" -C "$T" .
ls -lh "$OUT"
