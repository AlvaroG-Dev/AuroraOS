#!/bin/bash
# Saltos de tiempo > ${2:-0.3}s entre lineas consecutivas del log serie, solo hasta t<=${3:-20}s.
L=${1:-/tmp/nvboot.log}
grep -a '^\[ *[0-9.]*\]' $L | awk -v th=${2:-0.3} -v mx=${3:-20} '{t=substr($1,2)+0; if ($1=="[") t=substr($2,1,length($2)-1)+0; if (t>mx) exit; if (t-p>th) printf "%7.2f -> %7.2f (+%.1f)  %s\n", p, t, t-p, substr($0,1,130); p=t}'
n=$(grep -a -n "WM listo" $L | head -1 | cut -d: -f1)
echo "WM listo en linea $n; contexto:"; sed -n "$((n-3)),$((n+3))p" $L | cut -c1-130
