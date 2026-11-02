#!/bin/bash
# G3.3 (Plan_GCC_Autocompilacion.md): compilar GNU make 4.4.1 DENTRO de AuroraOS con el
# gcc nativo (Ubuntu) sobre ext2: ./configure + make, y usar el make recien hecho para
# reconstruirse a si mismo. QEMU sin KVM: el reloj del guest va ~8x mas lento que el real.
#   T=segundos (por defecto 7200) tests/gcc/run_make_test.sh
set -u
cd "$(dirname "$0")/../.."
SIZE=${SIZE:-1536}; T=${T:-14400}
D=build/devtools/root; W=build/gcc-test; C=build/gcc-cache; LOG=${LOG:-/tmp/gcc-make.log}
MK=make-4.4.1; SHA=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
[ -f $D/usr/bin/x86_64-linux-gnu-gcc-13 ] || { echo "falta $D: ejecuta scripts/fetch-devtools.sh"; exit 2; }
mkdir -p $C
[ -f $C/$MK.tar.gz ] || curl -fsSL -o $C/$MK.tar.gz https://ftp.gnu.org/gnu/make/$MK.tar.gz
echo "$SHA  $C/$MK.tar.gz" | sha256sum -c - >/dev/null || { echo "sha256 de $MK incorrecto"; exit 2; }
rm -rf $W; mkdir -p $W/tree/work
cp -a $D/usr $W/tree/usr
for f in libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1 libutil.so.1 libresolv.so.2 libgcc_s.so.1; do
  for src in libs/glibc/lib/x86_64-linux-gnu/$f /lib/x86_64-linux-gnu/$f; do
    [ -e $src ] && { cp -aL $src $W/tree/usr/lib/x86_64-linux-gnu/$f; break; }; done; done
mkdir -p $W/tree/usr/lib64; cp -aL libs/glibc/lib64/ld-linux-x86-64.so.2 $W/tree/usr/lib64/
ln -s usr/lib $W/tree/lib; ln -s usr/lib64 $W/tree/lib64
cp $C/$MK.tar.gz $W/tree/work/
mkdir -p $W/tree/tc
# gcc/cc con las rutas del toolchain de /data (el sysroot es /data)
cat > $W/tree/tc/gcc <<'S'
#!/bin/sh
export LD_LIBRARY_PATH=/data/usr/lib/x86_64-linux-gnu
exec /data/usr/bin/x86_64-linux-gnu-gcc-13 --sysroot=/data -B/data/usr/libexec/gcc/x86_64-linux-gnu/13/ -B/data/usr/lib/gcc/x86_64-linux-gnu/13/ "$@"
S
chmod +x $W/tree/tc/gcc; ln -s gcc $W/tree/tc/cc
cat > $W/tree/tc/build_make.sh <<'S'
#!/bin/sh
# Todo va al puerto serie (/dev/kmsg) para ver el progreso en directo.
export PATH=/data/tc:/data/usr/bin:$PATH LD_LIBRARY_PATH=/data/usr/lib/x86_64-linux-gnu
export TMPDIR=/data/work/tmp; mkdir -p $TMPDIR
up() { echo "@@T $1 uptime=$(cut -d' ' -f1 /proc/uptime)"; }
cd /data/work
up tar-normal-inicio
mkdir -p tartest && cd tartest && tar xzf ../make-4.4.1.tar.gz; echo "@@R tar-normal rc=$? (antes: 61 EINVAL en directorios)"; cd ..; rm -rf tartest
up tar-inicio
tar -xzf make-4.4.1.tar.gz; echo "@@R tar rc=$?"
cd make-4.4.1
up configure-inicio
sh ./configure --build=x86_64-pc-linux-gnu --disable-nls --without-guile CFLAGS=-O1; echo "@@R configure rc=$?"
up build-inicio
sh ./build.sh; echo "@@R build.sh rc=$?"
up build-fin
ls -la make; ./make --version | head -1
up make-con-make-inicio
./make -j2; echo "@@R make-con-make rc=$?"
up fin
ls -la make
echo fin > /data/work/done
S
chmod +x $W/tree/tc/build_make.sh
IMG=$W/gcc.img; rm -f $IMG
mke2fs -q -F -t ext2 -b 4096 -I 256 -d $W/tree $IMG ${SIZE}M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG SKIP_ENV=1
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "$T" \
 "sleep ${SETTLE:-45}" \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?' \
 'rm -f /data/work/done; (sh /data/tc/build_make.sh > /dev/kmsg 2>&1) > /dev/null 2>&1 &' \
 'W() { while [ ! -e /data/work/done ]; do sleep 10; done; }; W' \
 'W() { while [ ! -e /data/work/done ]; do sleep 10; done; }; W' \
 'W() { while [ ! -e /data/work/done ]; do sleep 10; done; }; W' \
 'W() { while [ ! -e /data/work/done ]; do sleep 10; done; }; W' \
 'W() { while [ ! -e /data/work/done ]; do sleep 10; done; }; W' \
 'W() { while [ ! -e /data/work/done ]; do sleep 10; done; }; W' \
 'echo @@BUILD; echo @@END' \
 'umount /data; echo @@UM rc=$?' 'echo @@DONE' 'power off' >/dev/null 2>&1
sec() { tr -d '\r' < "$LOG" | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -v -E '^\[ *[0-9.]+\]'; }
grep -a '@@MOUNT' "$LOG" | tr -d '\r' | head -2
tr -d '\r' < "$LOG" | grep -a -E '^@@(T|R)|^make-4|GNU Make|Built|error|Error' | head -60
grep -a -q '@@DONE' "$LOG" || { echo "FALLO: no termino (timeout)"; exit 1; }
e2fsck -fn $IMG >/dev/null 2>&1; echo "e2fsck -fn rc=$?"
