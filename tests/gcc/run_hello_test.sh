#!/bin/bash
# G2/H1 (Plan_GCC_Autocompilacion.md): hello world dentro de AuroraOS (QEMU).
#  1) binarios C/C++ producidos por el CROSS x86_64-pc-aurora-gnu (host) -> se ejecutan.
#  2) gcc NATIVO de Ubuntu (scripts/fetch-devtools.sh) sobre un ext2 montado en /data -> compila y ejecuta.
# Requiere: build/devtools/root (fetch-devtools.sh) y, opcional, el cross en
# build/aurora-toolchain (scripts/build-aurora-toolchain.sh); TCROSS=ruta lo cambia.
#   SIZE=MB T=segundos tests/gcc/run_hello_test.sh
set -u
cd "$(dirname "$0")/../.."
SIZE=${SIZE:-768}; T=${T:-900}
D=build/devtools/root; W=build/gcc-test; LOG=${LOG:-/tmp/gcc-hello.log}
TCROSS=${TCROSS:-build/aurora-toolchain}
[ -f $D/usr/bin/x86_64-linux-gnu-gcc-13 ] || { echo "falta $D: ejecuta scripts/fetch-devtools.sh"; exit 2; }
rm -rf $W; mkdir -p $W/tree
cp -a $D/usr $W/tree/usr
mkdir -p $W/tree/hello
# Runtime de la ISO dentro del sysroot /data (ld necesita /lib/x86_64-linux-gnu/libc.so.6...)
for f in libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1 libutil.so.1 libresolv.so.2 libgcc_s.so.1; do
  for src in libs/glibc/lib/x86_64-linux-gnu/$f /lib/x86_64-linux-gnu/$f; do
    [ -e $src ] && { cp -aL $src $W/tree/usr/lib/x86_64-linux-gnu/$f; break; }; done; done
mkdir -p $W/tree/usr/lib64; cp -aL libs/glibc/lib64/ld-linux-x86-64.so.2 $W/tree/usr/lib64/
ln -s usr/lib $W/tree/lib; ln -s usr/lib64 $W/tree/lib64
cat > $W/tree/hello/h.c <<'C'
#include <stdio.h>
int main(void){
#ifdef __aurora__
  puts("hola __aurora__");
#endif
  puts("hola desde gcc");
  return 0; }
C
cat > $W/tree/hello/h.cc <<'C'
#include <iostream>
#include <vector>
int main(){ std::vector<int> v{1,2,3}; std::cout << "c++ ok " << v.size() << "\n"; }
C
if [ -x $TCROSS/bin/x86_64-pc-aurora-gnu-gcc ]; then
  A=$TCROSS/bin/x86_64-pc-aurora-gnu; export PATH=$(realpath $TCROSS)/bin:$PATH
  mkdir -p $W/tree/hello/lib
  x86_64-pc-aurora-gnu-gcc -O2 $W/tree/hello/h.c -o $W/tree/hello/cross_c
  x86_64-pc-aurora-gnu-g++ -O2 $W/tree/hello/h.cc -o $W/tree/hello/cross_cc
  x86_64-pc-aurora-gnu-gcc -static -O2 $W/tree/hello/h.c -o $W/tree/hello/cross_s
  cp -aL $(x86_64-pc-aurora-gnu-g++ -print-file-name=libstdc++.so.6) $(x86_64-pc-aurora-gnu-gcc -print-file-name=libgcc_s.so.1) $W/tree/hello/lib/
fi
# La orden de boot-tests se corta (~256 bytes): la logica del compilador va en un script de la imagen.
cat > $W/tree/hello/native.sh <<'S'
#!/bin/sh
export PATH=/data/usr/bin:$PATH LD_LIBRARY_PATH=/data/usr/lib/x86_64-linux-gnu
G=x86_64-linux-gnu-gcc-13
F="--sysroot=/data -B/data/usr/libexec/gcc/x86_64-linux-gnu/13/ -B/data/usr/lib/gcc/x86_64-linux-gnu/13/"
cd /tmp
echo == lectura ext2; time cat /data/usr/libexec/gcc/x86_64-linux-gnu/13/cc1 > /dev/null
time $G --version | head -1
time $G $F /data/hello/h.c -o /tmp/h_native; echo gcc_c rc=$?
/tmp/h_native; echo run_c rc=$?
time $G $F -static /data/hello/h.c -o /tmp/h_static; echo gcc_static rc=$?
/tmp/h_static; echo run_static rc=$?
G2=x86_64-linux-gnu-g++-13
time $G2 $F /data/hello/h.cc -o /tmp/h_cpp; echo gxx rc=$?
/tmp/h_cpp; echo run_cpp rc=$?
S
chmod +x $W/tree/hello/native.sh
IMG=$W/gcc.img; rm -f $IMG
mke2fs -q -F -t ext2 -b 4096 -I 256 -d $W/tree $IMG ${SIZE}M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG SKIP_ENV=1
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
L=/data/usr/lib/x86_64-linux-gnu
bash scripts/qemu-boot-test.sh -t "$T" \
 "sleep ${SETTLE:-45}" \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?' \
 'echo @@CROSS; /data/hello/cross_s; echo rc=$?; LD_LIBRARY_PATH=/data/hello/lib /data/hello/cross_c; echo rc=$?; LD_LIBRARY_PATH=/data/hello/lib /data/hello/cross_cc; echo rc=$?; echo @@END' \
 'echo @@NATIVE; sh /data/hello/native.sh 2>&1; echo @@END' \
 'umount /data; echo @@UM rc=$?' 'echo @@DONE' 'power off' >/dev/null 2>&1
sec() { tr -d '\r' < "$LOG" | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -v -E '^\[ *[0-9.]+\]'; }
echo "== mount:"; grep -a '@@MOUNT' "$LOG" | tr -d '\r' | head -2
echo "== cross:"; sec CROSS | head -12
echo "== nativo:"; sec NATIVE | head -40
grep -a -q '@@DONE' "$LOG" || { echo "FALLO: no termino (timeout)"; exit 1; }
