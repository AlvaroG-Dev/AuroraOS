#!/bin/sh
# Reconstruye bios.bin (SeaBIOS 1.16.3 + parches/*.patch) con la configuracion de ./config.
# Uso: third_party/seabios/build.sh   (necesita gcc, binutils, python3, make)
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
cd "$work"
curl -fsSL -o sb.tgz https://www.seabios.org/downloads/seabios-1.16.3.tar.gz
tar xzf sb.tgz
cd seabios-1.16.3
for p in "$here"/parches/*.patch; do patch -p1 < "$p"; done
cp "$here/config" .config
make olddefconfig >/dev/null
make -j"$(nproc)"
cp out/bios.bin "$here/bios.bin"
cp out/vgabios.bin "$here/vgabios.bin"
echo "vgabios.bin: $(wc -c < "$here/vgabios.bin") bytes"
echo "bios.bin actualizado ($(wc -c < "$here/bios.bin") bytes)"
