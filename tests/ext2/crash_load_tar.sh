#!/bin/bash
# Carga tipo "extraer fuentes + compilar": tar x en directorios nuevos, "objetos", borrados.
cd /data/w || exit 1
i=0
while true; do
  i=$((i+1)); t=x$((i%3)); rm -rf $t; mkdir -p $t
  tar xf /data/stable/src.tar -C $t
  for k in $(seq 1 60); do head -c $((RANDOM*2+3000)) /dev/urandom > $t/o$k.o; done
  [ $((i%2)) = 0 ] && rm -rf x$(((i+1)%3))
done
