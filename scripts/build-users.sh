#!/bin/bash
# Compila las herramientas de cuentas (login, su, passwd, useradd, sudo) contra
# la glibc del host (la misma que va en la ISO) y las instala en sysroot/usr/bin.
# su/passwd/sudo/login quedan setuid-root (4755). Sin busybox ni PAM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/user/aurora-users/aurora-users.c"
OUT="$ROOT/sysroot/usr/bin"
mkdir -p "$OUT"
gcc -O2 -Wall -Wextra -Wno-unused-result -o "$OUT/aurora-users" "$SRC" -lcrypt
for t in login su passwd sudo; do
  cp -f "$OUT/aurora-users" "$OUT/aurora-$t"
  chmod 4755 "$OUT/aurora-$t"
done
cp -f "$OUT/aurora-users" "$OUT/aurora-useradd"
chmod 0755 "$OUT/aurora-useradd" "$OUT/aurora-users"
# Nombres habituales (el binario decide por basename). Sin busybox.
for t in login su passwd sudo; do
  cp -f "$OUT/aurora-users" "$OUT/$t"
  chmod 4755 "$OUT/$t"
done
cp -f "$OUT/aurora-users" "$OUT/useradd"
chmod 0755 "$OUT/useradd"
echo "[Makefile]   aurora-users: login su passwd sudo (setuid) + useradd"
