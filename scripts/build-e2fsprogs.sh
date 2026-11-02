#!/bin/bash
# Compila e2fsprogs (estatico, glibc del host) e instala mke2fs, e2fsck,
# tune2fs, dumpe2fs y debugfs en precompiled/usr/sbin (+ enlaces mkfs.ext2,
# fsck.ext2, e2label...). Uso: scripts/build-e2fsprogs.sh [version]
set -euo pipefail
cd "$(dirname "$0")/.."
VER=${1:-1.47.0}
W=${E2BUILD:-/tmp/aurora-e2fsprogs}
mkdir -p "$W/build"
if [ ! -d "$W/e2fsprogs-$VER" ]; then
  [ -f "$W/e2fsprogs-$VER.tar.xz" ] || curl -fsSL -o "$W/e2fsprogs-$VER.tar.xz" \
    "https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v$VER/e2fsprogs-$VER.tar.xz"
  tar -C "$W" -xf "$W/e2fsprogs-$VER.tar.xz"
fi
# uname() de Aurora da release "0.1.0": mke2fs creeria FS rev 0 (sin features)
# por creer que el "kernel es anterior a 2.2". Es siempre Linux-like: sin chequeo.
sed -i 's/is_before_linux_ver([0-9, ]*)/0/g' "$W/e2fsprogs-$VER/misc/mke2fs.c"
# El recogedor de senales de e2fsck (backtrace) entra en bucle con SIGSEGV en Aurora.
sed -i 's/^\tsigcatcher_setup();/\t\/* sigcatcher_setup(); *\//' "$W/e2fsprogs-$VER/e2fsck/unix.c"
cd "$W/build"
[ -f Makefile ] || "$W/e2fsprogs-$VER/configure" --disable-nls --disable-fuse2fs \
  --disable-defrag --disable-uuidd --disable-e2initrd-helper --disable-libblkid \
  --disable-libuuid --disable-elf-shlibs --disable-bsd-shlibs \
  --without-libiconv-prefix LDFLAGS=-static CFLAGS="-O2" >configure.log 2>&1
# Sin hilos: e2fsck los usa en la fase 5 (HAVE_PTHREAD) y el kernel de Aurora
# aun no los aguanta ahi (desbordamiento de pila al terminar).
if grep -q '^D\["HAVE_PTHREAD"\]' config.status; then
  sed -i '/^D\["HAVE_PTHREAD"\]/d; /^D\["HAVE_PTHREAD_H"\]/d' config.status
  ./config.status lib/config.h >/dev/null
fi
make -j"$(nproc)" >make.log 2>&1
DEST="$OLDPWD/precompiled/usr/sbin"
mkdir -p "$DEST"
for b in e2fsck/e2fsck misc/mke2fs misc/tune2fs misc/dumpe2fs debugfs/debugfs; do
  install -m 755 -s "$b" "$DEST/$(basename "$b")"
done
cd "$DEST"
for l in mkfs.ext2 mkfs.ext3 mkfs.ext4; do ln -sf mke2fs "$l"; done
for l in fsck.ext2 fsck.ext3 fsck.ext4; do ln -sf e2fsck "$l"; done
ln -sf tune2fs e2label
echo "[e2fsprogs] instalado en $DEST"
