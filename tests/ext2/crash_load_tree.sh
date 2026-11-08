#!/bin/bash
# Carga tipo compilacion: varios trabajadores en paralelo crean arboles de
# directorios (algunos grandes), los copian, compilan "objetos" y borran arboles.
cd /data/w || exit 1
worker() {
  local w=$1 i=0
  while true; do
    i=$((i+1)); t=t$w
    mkdir -p $t/src/a/b $t/obj
    for k in $(seq 1 $((RANDOM%400+100))); do echo "$k $i" > $t/src/a/f$k.c; done
    for k in $(seq 1 30); do seq 1 $((RANDOM%3000)) > $t/src/a/b/h$k.h; done
    cp -r $t/src $t/src2 2>/dev/null
    for k in $(seq 1 40); do head -c $((RANDOM*3+4000)) /dev/urandom > $t/obj/o$k.o; done
    [ $((i%2)) = 0 ] && rm -rf $t/src2
    [ $((i%3)) = 0 ] && rm -rf $t/obj
    [ $((i%4)) = 0 ] && rm -rf $t/src
  done
}
for w in 1 2 3; do worker $w & done
wait
