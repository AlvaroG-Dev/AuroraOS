#!/bin/bash
# Busca el cuelgue intermitente de `sleep`: una carga de CPU en segundo plano y
# cientos de sleeps cortos; si alguno no vuelve, el contador deja de avanzar.
klog() { echo "SSLOG $*" > /dev/kmsg; }
( while :; do :; done ) &
HOG=$!
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
for i in $(seq 1 ${SS_N:-150}); do
  sleep 0.3
  [ $((i % 10)) -eq 0 ] && klog "iter $i"
done
klog "TERMINADO $i iteraciones"
kill $HOG $APID 2>/dev/null
echo DRIVE-DONE
