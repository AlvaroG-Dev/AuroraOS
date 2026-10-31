#!/bin/bash
# Lo que libdrm/Mesa leen para identificar la GPU de un nodo DRM.
for m in 226:0 226:128; do
  d=/sys/dev/char/$m
  echo "DRMSYS $m link=$(readlink $d 2>&1)"
  for f in vendor device subsystem_vendor subsystem_device revision; do
    echo "DRMSYS $m $f=$(cat $d/device/$f 2>&1 | head -1)"
  done
  echo "DRMSYS $m config=$(head -c 16 $d/device/config 2>&1 | od -An -tx1 | tr -d "\n")"
  echo "DRMSYS $m subsystem=$(readlink $d/device/subsystem 2>&1)"
done
echo "DRMSYS chars=$(ls /sys/dev/char 2>&1 | tr '\n' ' ')"
echo DRIVE-DONE
