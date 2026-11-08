#!/bin/bash
# scripts/check-sysroot-libc.sh — comprueba que la glibc del sysroot es la de
# libs/glibc (ld.so + libc.so.6 + libm + libgcc_s...) y no una copia del host.
# Una libc del host de otra version junto al ld.so de libs/glibc rompe todos los
# programas dinamicos (los .aur mueren al arrancar con un PF en el vDSO).
# Salida: 0 si todo coincide (o no hay libs/glibc), 1 y mensaje si no.
cd "$(dirname "$0")/.." || exit 1
[ -d libs/glibc ] || exit 0
bad=0
while IFS= read -r f; do
    rel="${f#libs/glibc/}"
    for dst in "sysroot/$rel" "sysroot/usr/$rel"; do
        [ -f "$dst" ] || continue
        if ! cmp -s "$f" "$dst"; then
            echo "[check-libc] ERROR: $dst difiere de $f" >&2
            bad=1
        fi
    done
done < <(find libs/glibc \( -name 'libc.so.6' -o -name 'libm.so.6' -o -name 'ld-linux-x86-64.so.2' \
         -o -name 'libpthread.so.0' -o -name 'libdl.so.2' -o -name 'librt.so.1' -o -name 'libgcc_s.so.1' \) -type f)
if [ $bad -ne 0 ]; then
    echo "[check-libc] La glibc del sysroot no coincide con libs/glibc (se coló una libc del host)." >&2
    echo "[check-libc] Solucion: rm -rf sysroot/lib sysroot/lib64 sysroot/usr/lib/x86_64-linux-gnu y repetir make." >&2
    exit 1
fi
exit 0
