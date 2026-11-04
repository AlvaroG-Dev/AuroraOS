#!/bin/bash
# Herramientas de desarrollo YA DENTRO de la ISO (WITH_DEVTOOLS=1) + /tmp grande.
#   T=segundos tests/gcc/run_iso_tools_test.sh
set -u
cd "$(dirname "$0")/../.."
T=${T:-900}; LOG=${LOG:-/tmp/iso-tools.log}
export WITH_NVIDIA=${WITH_NVIDIA:-0} LOG SKIP_ENV=1
bash scripts/qemu-boot-test.sh -t "$T" \
 "sleep ${SETTLE:-30}" \
 'echo @@T; df /tmp | tail -1; for t in gcc g++ make nasm mcopy mkfs.fat xorriso sfdisk sgdisk xxd gawk bison flex m4 bc patch as ld; do command -v $t >/dev/null && echo ok $t || echo FALTA $t; done; echo @@END' \
 'echo @@BIG; dd if=/dev/zero of=/tmp/big bs=1M count=300 2>&1 | tail -1; ls -l /tmp/big; rm /tmp/big; echo @@END' \
 'echo @@CC; cd /tmp; printf "#include <stdio.h>\nint main(){puts(\"hola gcc iso\");return 0;}\n" > h.c; gcc h.c -o h && ./h; echo rc=$?; echo @@END' \
 'echo @@FAT; cd /tmp; dd if=/dev/zero of=f.img bs=1M count=40 2>/dev/null; mkfs.fat -F 32 f.img >/dev/null; mmd -i f.img ::/E; mcopy -i f.img h.c ::/E/H.C; mdir -i f.img ::/E; echo rc=$?; echo @@END' \
 'sleep 20' \
 'echo @@DONE' 'power off' >/dev/null 2>&1
sec() { tr -d '\r' < "$LOG" | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -v -E '^\[ *[0-9.]+\]'; }
for s in T BIG CC FAT; do echo "== $s"; sec $s | head -30; done
grep -a -q '@@DONE' "$LOG" || { echo "FALLO: no termino (timeout)"; exit 1; }
