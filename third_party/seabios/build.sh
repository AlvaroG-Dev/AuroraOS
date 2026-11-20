#!/bin/sh
# Reconstruye bios.bin (SeaBIOS 1.16.3, sin parches) con la configuracion de ./config.
# Uso: third_party/seabios/build.sh   (necesita gcc, binutils, python3, make)
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
cd "$work"
curl -fsSL -o sb.tgz https://www.seabios.org/downloads/seabios-1.16.3.tar.gz
tar xzf sb.tgz
cd seabios-1.16.3
cp "$here/config" .config
make olddefconfig >/dev/null
make -j"$(nproc)"
cp out/bios.bin "$here/bios.bin"
echo "bios.bin actualizado ($(wc -c < "$here/bios.bin") bytes)"
