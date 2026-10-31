#!/bin/bash
L=${1:-/tmp/nvboot.log}
echo "exports DEBUG: $(grep -ac 'DEBUG \[KUR\] export' $L)"
echo "--- hitos"
grep -a -n "cargado en slot\|leído\|WM listo\|\[nvidia\] \|NVRM\|Initialized\|kms:\|\[LKPI\]\|nvidia-drm\|desktop\|escritorio" $L | cut -c1-160 | awk -F: '$1>300' | head -70
echo "--- cola"
tail -4 $L | cut -c1-160
