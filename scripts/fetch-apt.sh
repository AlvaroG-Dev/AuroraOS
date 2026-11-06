#!/bin/bash
# scripts/fetch-apt.sh — APT + dpkg de Ubuntu 24.04 (noble), la misma glibc 2.39 de
# la ISO, SIN root: baja los .deb con ubuntu-fetch-debs.py y los extrae en
# build/apt/root. El Makefile los instala con aurora-apt (envoltorios que
# mueven todo el estado a /usr/lib/aurora-apt, que en el sistema instalado vive en
# el ext2: /etc, /var y /tmp son tmpfs y no persisten).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/apt"; DEBS="$OUT/debs"; TREE="$OUT/root"
mkdir -p "$DEBS" "$TREE"; cd "$DEBS"
# Lo que ya trae la ISO (libc6, bash, coreutils...) no se duplica.
SKIP='^(libc6|libgcc-s1|libstdc\+\+6|bash|dash|coreutils|debconf|perl-base|base-files|gcc-14-base|gcc-13-base|debianutils|sed|grep|tar|gzip|findutils|zlib1g|libssl3t64|libacl1|libattr1|libcrypt1|libtinfo6|ncurses-base|ncurses-bin|mawk|util-linux|libblkid1|libmount1|libuuid1|libsmartcols1|libpam0g|libpam-modules|libpam-runtime|libaudit1|libcap-ng0|login|passwd|diffutils|hostname|init-system-helpers|lsb-base|sysvinit-utils|libc-bin|ubuntu-keyring-dummy)$'
python3 "$ROOT/scripts/ubuntu-fetch-debs.py" "$DEBS" "$SKIP" apt dpkg gpgv ubuntu-keyring
for d in *.deb; do dpkg-deb -x "$d" "$TREE"; done
rm -rf "$TREE/usr/share/doc" "$TREE/usr/share/man" "$TREE/usr/share/locale" \
       "$TREE/usr/share/lintian" "$TREE/usr/share/bug" "$TREE/etc/cron.daily" \
       "$TREE/lib/systemd" "$TREE/usr/lib/systemd" "$TREE/usr/share/bash-completion"
for d in sbin bin lib; do [ -d "$TREE/$d" ] && [ ! -L "$TREE/$d" ] && { mkdir -p "$TREE/usr/$d"; cp -a --update=none "$TREE/$d/." "$TREE/usr/$d/"; rm -rf "$TREE/$d"; }; done
echo "[apt] extraido en $TREE ($(du -sh "$TREE" | cut -f1))"
[ -x "$TREE/usr/bin/apt-get" ] && [ -x "$TREE/usr/bin/dpkg" ] || { echo "[apt] FALTA apt-get/dpkg"; exit 1; }
