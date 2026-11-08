#!/bin/bash
# scripts/build-aurora-toolchain.sh — Cross-compilador x86_64-pc-aurora-gnu en el host
# (Plan_GCC_Autocompilacion.md, Escalon B). binutils 2.42 + GCC 13.3.0 (C, C++)
# parcheados por scripts/aurora-toolchain-patch.sh, contra el sysroot de
# scripts/mk-aurora-sysroot.sh. Resultado en build/aurora-toolchain/{bin,lib,...}.
# Requisitos: scripts/fetch-devtools.sh ya ejecutado; red para bajar fuentes.
# Uso: scripts/build-aurora-toolchain.sh [-jN]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS=${1:--j$(nproc)}
T=x86_64-pc-aurora-gnu
W="$ROOT/build/aurora-tc-work"; PREFIX="$ROOT/build/aurora-toolchain"; SYSROOT="$ROOT/build/aurora-sysroot"
BU_V=2.42; GCC_V=13.3.0
mkdir -p "$W/src" "$W/b-bu" "$W/b-gcc"
cd "$W/src"
[ -f binutils-$BU_V.tar.xz ] || curl -fsSLO https://ftp.gnu.org/gnu/binutils/binutils-$BU_V.tar.xz
[ -f gcc-$GCC_V.tar.xz ] || curl -fsSLO https://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC_V/gcc-$GCC_V.tar.xz
# Hashes: anotar aqui tras la primera descarga verificada (pendiente de fijar).
if [ ! -d patched ]; then
    mkdir patched; tar xf binutils-$BU_V.tar.xz -C patched; tar xf gcc-$GCC_V.tar.xz -C patched
    (cd patched/gcc-$GCC_V && ./contrib/download_prerequisites --no-isl)
    "$ROOT/scripts/aurora-toolchain-patch.sh" "$PWD/patched/binutils-$BU_V" "$PWD/patched/gcc-$GCC_V"
fi
"$ROOT/scripts/mk-aurora-sysroot.sh" "$SYSROOT"
export PATH="$PREFIX/bin:$PATH"

cd "$W/b-bu"
[ -f Makefile ] || "$W/src/patched/binutils-$BU_V/configure" --prefix="$PREFIX" --target=$T \
    --with-sysroot="$SYSROOT" --disable-nls --disable-gprofng --disable-werror --disable-gdb \
    --disable-sim --disable-libdecnumber --disable-readline
make $JOBS MAKEINFO=true && make install MAKEINFO=true

cd "$W/b-gcc"
[ -f Makefile ] || "$W/src/patched/gcc-$GCC_V/configure" --prefix="$PREFIX" --target=$T \
    --with-sysroot="$SYSROOT" --enable-languages=c,c++ --disable-multilib --enable-multiarch \
    --disable-nls --disable-bootstrap --disable-libsanitizer --disable-libvtv --disable-libquadmath \
    --disable-libssp --disable-libgomp --disable-libatomic --disable-libitm --disable-lto \
    --enable-default-pie --enable-shared --enable-threads=posix --enable-__cxa_atexit \
    --enable-initfini-array --without-isl
make $JOBS all-gcc MAKEINFO=true && make install-gcc MAKEINFO=true
make $JOBS all-target-libgcc MAKEINFO=true && make install-target-libgcc MAKEINFO=true
make $JOBS all-target-libstdc++-v3 MAKEINFO=true && make install-target-libstdc++-v3 MAKEINFO=true
echo "[toolchain] listo: $PREFIX/bin/$T-gcc"
