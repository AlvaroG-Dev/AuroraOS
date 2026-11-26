#!/bin/sh
# Perfil del kernel (/proc/aurora-prof) durante un `wine cmd /c exit` en
# caliente y en frio: que syscalls y que fallos de pagina se llevan el tiempo.
export HOME=/tmp/winehome WINEPREFIX=/tmp/winehome/.wine
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/0
export AURORA_WINE_LOG=/tmp/aurora-wine.log WINEDEBUG=-all
mkdir -p "$HOME"
echo "WINE-PROF: inicio"
sleep ${WAIT_DESKTOP:-60}
timeout 900 aurora-wine --setup </dev/null >/dev/null 2>&1; echo "WINE-PROF: setup rc=$?"
wineserver -k; timeout 30 wineserver -w; sleep 2
dif() { # antes despues
  awk 'NR==FNR{a[$1]=$2; b[$1]=$3; next} ($1 in a){c=$2-a[$1]; u=$3-b[$1]; if (c>0) printf "%s %d %d\n",$1,c,u; next} {if ($2+0>0) print $1,$2,$3}' "$1" "$2" | sort -k3 -n -r | head -n 25
}
wineserver -p 600
cat /proc/aurora-prof > /tmp/p0
t0=$(date +%s%N); wine cmd /c exit; t1=$(date +%s%N)
cat /proc/aurora-prof > /tmp/p1
echo "WINE-PROF: frio $(( (t1-t0)/1000000 )) ms"; dif /tmp/p0 /tmp/p1
t0=$(date +%s%N); wine cmd /c exit; t1=$(date +%s%N)
cat /proc/aurora-prof > /tmp/p2
echo "WINE-PROF: caliente $(( (t1-t0)/1000000 )) ms"; dif /tmp/p1 /tmp/p2
for f in /tmp/p0 /tmp/p1 /tmp/p2; do echo "== $f"; cat $f; done
echo "WINE-PROF: fin"
