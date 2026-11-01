#!/bin/bash
# Arranca AuroraOS en QEMU con una imagen ext2 como disco AHCI (se monta sola
# en /data, solo lectura) y compara stat + sha256 de todo el árbol con el
# manifiesto generado por gen_images.sh.
#   tests/ext2/run_read_test.sh <imagen> [directorio_de_gen_images]
set -u
cd "$(dirname "$0")/../.."
IMG=$(readlink -f "$1"); DIR=${2:-build/ext2-test}
NAME=$(basename "$IMG" .img)
LOG=${LOG:-/tmp/ext2-read-$NAME.log}
export WITH_NVIDIA=${WITH_NVIDIA:-0}
export QEMU_EXTRA="-drive file=$IMG,if=none,id=e2,format=raw,snapshot=on -device ahci,id=ahci0 -device ide-hd,drive=e2,bus=ahci0.0"
export LOG
bash scripts/qemu-boot-test.sh -t "${T:-300}" \
 "sleep ${SETTLE:-60}" \
 "echo @@MOUNT; grep -a ' /data ' /proc/self/mountinfo; echo @@END" \
 "cd /data && echo @@STAT && find . -mindepth 1 -path ./many -prune -o -printf '%p %y %m %U %G %s %n\\n'; echo @@END" \
 "cd /data && echo @@STAT2 && find . -mindepth 1 -path ./many -prune -o -printf '%p %y %m %U %G %s %n\\n'; echo @@END" \
 "cd /data && echo @@SHA && find . -type f ! -path './many/*' -exec sha256sum {} \\; ; sha256sum ./many/entry_0000? ./many/entry_02999; echo @@END" \
 "cd /data && echo @@LINKS && for n in 1 58 59 60 61 100 300; do readlink links/sym\$n | wc -c; done; cat links/rel | sha256sum; p=links/dirlink; for i in \$(seq 0 29); do p=\$p/d\$i; done; cat \$p/hoja; echo @@END" \
 "cd /data && echo @@LS && ls many | wc -l && ls -f many | wc -l; echo @@END" \
 "cd /data && echo @@RO && (echo x > nuevo; echo rc=\$?) 2>&1; (rm sizes/f1; echo rc=\$?) 2>&1; (touch sizes/f0; echo rc=\$?) 2>&1; echo @@END" \
 "echo @@DONE" "power off" >/dev/null 2>&1
section() { tr -d '\r' < "$LOG" | awk -v s="@@$1" '$0==s{f=1;next} f&&$0=="@@END"{exit} f' | grep -a -v -E '^\[ *[0-9.]+\]'; }
# Directorios: tamaño y nlink dependen del FS donde se generó el árbol.
norm() { awk '{ if ($2=="d") { $6=0; $7=0 } print }'; }
fail=0
echo "== $NAME: $(grep -a -E "\[EXT2\] (montado|.*no soportad|.*necesita|.*inválid)" "$LOG" | head -2 | cut -c1-200 | tr '\n' ' ')"
if ! grep -a -q '@@DONE' "$LOG"; then echo "FALLO: la prueba no terminó (¿cuelgue/timeout?)"; exit 1; fi
# El log del kernel comparte la consola y a veces se mezcla con una línea:
# se hace el listado dos veces y se unen solo las líneas bien formadas.
okline() { grep -a -E '^\./\S+ [a-z] [0-7]+ [0-9]+ [0-9]+ [0-9]+ [0-9]+$' | grep -a -v '^\./lost+found'; }
{ section STAT; section STAT2; } | okline | norm | LC_ALL=C sort -u > /tmp/e2_stat_$NAME.txt
# many/ (3000 entradas) se comprueba aparte por recuento: su listado dispara
# el aviso [GETDENTS] del kernel, que se mezcla con la salida.
grep -a -v -E '^\./many( |/)' "$DIR/manifest.stat" | norm | LC_ALL=C sort -u > /tmp/e2_stat_exp.txt
if diff /tmp/e2_stat_exp.txt /tmp/e2_stat_$NAME.txt > /tmp/e2_stat_$NAME.diff; then
  echo "stat: OK ($(wc -l < /tmp/e2_stat_$NAME.txt) entradas)"
else echo "stat: DIFERENCIAS ($(wc -l < /tmp/e2_stat_$NAME.diff) líneas), ver /tmp/e2_stat_$NAME.diff"; head -6 /tmp/e2_stat_$NAME.diff; fail=1; fi
section SHA | LC_ALL=C sort -k2 > /tmp/e2_sha_$NAME.txt
# sha256 solo de una muestra de many/ (un fork por fichero es muy lento en TCG)
awk '$2 !~ /^\.\/many\// || $2 ~ /entry_0000[0-9]$|entry_02999$/' "$DIR/manifest.sha" | LC_ALL=C sort -k2 > /tmp/e2_sha_exp.txt
if diff /tmp/e2_sha_exp.txt /tmp/e2_sha_$NAME.txt > /tmp/e2_sha_$NAME.diff; then
  echo "sha256: OK ($(wc -l < /tmp/e2_sha_$NAME.txt) ficheros)"
else echo "sha256: DIFERENCIAS ($(wc -l < /tmp/e2_sha_$NAME.diff) líneas), ver /tmp/e2_sha_$NAME.diff"; head -6 /tmp/e2_sha_$NAME.diff; fail=1; fi
echo "-- enlaces:"; section LINKS | tr '\n' '|'; echo
echo "-- many:"; section LS | tr '\n' '|'; echo
echo "-- RO:"; section RO | tr '\n' '|'; echo
exit $fail
