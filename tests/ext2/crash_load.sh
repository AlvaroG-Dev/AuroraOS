#!/bin/bash
# Carga aleatoria de metadatos y datos sobre /data/w (para los cortes de energia).
cd /data/w || exit 1
i=0
while true; do
  i=$((i+1)); d=d$((RANDOM%7)); mkdir -p $d
  seq 1 $((RANDOM%4000)) > $d/f$((RANDOM%40))
  [ $((i%3)) = 0 ] && rm -f $d/f$((RANDOM%40))
  [ $((i%4)) = 0 ] && mv -f $d/f$((RANDOM%40)) $d/g$((RANDOM%40)) 2>/dev/null
  [ $((i%7)) = 0 ] && dd if=/dev/zero of=$d/big$((RANDOM%4)) bs=1k count=$((RANDOM%600)) 2>/dev/null
  [ $((i%9)) = 0 ] && rm -rf d$((RANDOM%7))
  [ $((i%5)) = 0 ] && ln -sf f1 $d/sl$((RANDOM%5))
  [ $((i%6)) = 0 ] && ln -f $d/f1 $d/hl$((RANDOM%5)) 2>/dev/null
  [ $((i%8)) = 0 ] && : > $d/f$((RANDOM%40))
done
