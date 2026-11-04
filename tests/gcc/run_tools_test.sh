#!/bin/bash
# Punto 3 (Plan_GCC_Autocompilacion.md): herramientas de empaquetado de la ISO
# dentro de AuroraOS: nasm, mkfs.fat, mtools (mmd/mcopy), xorriso, sfdisk/sgdisk,
# xxd, gawk, bison/flex, m4. Requiere build/devtools/root (scripts/fetch-devtools.sh).
#   SIZE=MB T=segundos tests/gcc/run_tools_test.sh
set -u
cd "$(dirname "$0")/../.."
SIZE=${SIZE:-512}; T=${T:-900}
D=build/devtools/root; W=build/gcc-test; LOG=${LOG:-/tmp/gcc-tools.log}
[ -x $D/usr/bin/nasm ] || { echo "falta $D: ejecuta scripts/fetch-devtools.sh"; exit 2; }
rm -rf $W; mkdir -p $W/tree/t
cp -a $D/usr $W/tree/usr
[ -d $W/tree/usr/lib/x86_64-linux-gnu/gconv ] || cp -a /usr/lib/x86_64-linux-gnu/gconv $W/tree/usr/lib/x86_64-linux-gnu/gconv
for f in libc.so.6 libm.so.6 libmvec.so.1 libdl.so.2 libpthread.so.0 librt.so.1 libutil.so.1 libresolv.so.2 libgcc_s.so.1; do
  for src in libs/glibc/lib/x86_64-linux-gnu/$f /lib/x86_64-linux-gnu/$f; do
    [ -e $src ] && { cp -aL $src $W/tree/usr/lib/x86_64-linux-gnu/$f; break; }; done; done
mkdir -p $W/tree/usr/lib64; cp -aL libs/glibc/lib64/ld-linux-x86-64.so.2 $W/tree/usr/lib64/
ln -s usr/lib $W/tree/lib; ln -s usr/lib64 $W/tree/lib64
printf 'bits 64\nglobal _start\n_start: mov eax,60\n xor edi,edi\n syscall\n' > $W/tree/t/a.asm
echo "contenido de prueba" > $W/tree/t/f.txt
cat > $W/tree/t/tools.sh <<'S'
#!/bin/sh
export PATH=/data/usr/bin:/data/usr/sbin:$PATH LD_LIBRARY_PATH=/data/usr/lib/x86_64-linux-gnu GCONV_PATH=/data/usr/lib/x86_64-linux-gnu/gconv
cd /tmp
chk() { n=$1; shift; "$@" >/tmp/o.$n 2>&1; r=$?; echo "T $n rc=$r"; [ $r -eq 0 ] || head -3 /tmp/o.$n; }
chk nasm      nasm -f elf64 /data/t/a.asm -o /tmp/a.o
df /tmp | tail -1
chk dd        dd if=/dev/zero of=/tmp/f.img bs=1M count=20
chk mkfs.fat  mkfs.fat -F 32 -v /tmp/f.img
chk mmd       mmd -i /tmp/f.img ::/EFI
chk mcopy     mcopy -i /tmp/f.img /data/t/f.txt ::/EFI/F.TXT
chk mdir      mdir -i /tmp/f.img ::/EFI
chk xorriso   xorriso -as mkisofs -o /tmp/t.iso /data/t
chk xxd       sh -c 'echo 55aa | xxd -r -p > /tmp/m.bin; test "$(wc -c < /tmp/m.bin)" = 2'
chk gawk      sh -c 'echo 3 4 | gawk "{print \$1*\$2}" | grep -q 12'
chk sfdisk    sh -c 'echo "label: dos
start=2048, size=20000, type=c" | sfdisk /tmp/f.img'
chk sgdisk    sgdisk -p /tmp/f.img
chk m4        sh -c 'echo "define(X,5)X" | m4 | grep -q 5'
chk bison     bison --version
chk flex      flex --version
ls -l /tmp/t.iso /tmp/f.img
S
chmod +x $W/tree/t/tools.sh
IMG=$W/tools.img; rm -f $IMG
mke2fs -q -F -t ext2 -b 4096 -I 256 -d $W/tree $IMG ${SIZE}M || exit 2
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG SKIP_ENV=1
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
bash scripts/qemu-boot-test.sh -t "$T" \
 "sleep ${SETTLE:-45}" \
 'umount /data; mount -t ext2 /dev/sda /data; echo @@MOUNT rc=$?' \
 'echo @@TOOLS; sh /data/t/tools.sh 2>&1; echo @@END' \
 'umount /data; echo @@UM rc=$?' 'echo @@DONE' 'power off' >/dev/null 2>&1
sec() { tr -d '\r' < "$LOG" | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -v -E '^\[ *[0-9.]+\]'; }
echo "== mount:"; grep -a '@@MOUNT' "$LOG" | tr -d '\r' | head -2
echo "== herramientas:"; sec TOOLS | head -40
grep -a -q '@@DONE' "$LOG" || { echo "FALLO: no termino (timeout)"; exit 1; }
