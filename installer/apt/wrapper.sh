#!/bin/sh
# Envoltorio: apunta apt/dpkg al estado de AuroraOS y ejecuta el binario real de
# /usr/lib/aurora-apt/bin. Estado persistente: /usr/lib/aurora-apt (ext2 instalado).
# En la ISO en vivo /usr es de solo lectura: el estado va a /var/lib/aurora-apt-state
# (tmpfs /var, 1/4 de la RAM; /run es de solo 4 MB y no cabe) y solo funcionan update/search/show (no se puede instalar en una raiz ro).
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
B=/usr/lib/aurora-apt
S=$B
if ! mkdir -p $B/.rw 2>/dev/null; then
  S=/var/lib/aurora-apt-state
  if [ ! -f $S/apt.conf ]; then
    mkdir -p $S
    cp -a $B/etc $B/dpkg $S/ 2>/dev/null
    sed "s|/usr/lib/aurora-apt|/var/lib/aurora-apt-state|g" $B/apt.conf > $S/apt.conf
  fi
fi
mkdir -p /run/aurora-apt $S/state/lists/partial $S/cache/archives/partial $S/dpkg/updates $S/dpkg/info $S/dpkg/alternatives 2>/dev/null
export APT_CONFIG=$S/apt.conf
export DPKG_ADMINDIR=$S/dpkg
n=$(basename "$0")
case "$n" in dpkg|dpkg-query|dpkg-divert|dpkg-statoverride|dpkg-trigger)
  set -- "--admindir=$S/dpkg" "$@" ;; esac
exec $B/bin/"$n" "$@"
