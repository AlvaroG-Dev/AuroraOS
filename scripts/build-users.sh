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
# Aviso si el binario exige una glibc mas nueva que la de la ISO (libs/glibc).
need=$(objdump -T "$OUT/aurora-users" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)
have=$(strings -a "$ROOT/libs/glibc/lib/x86_64-linux-gnu/libc.so.6" 2>/dev/null | sed -n 's/.*release version \([0-9][0-9.]*[0-9]\).*/\1/p' | head -1)
if [ -n "$need" ] && [ -n "$have" ] && [ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | tail -1)" != "$have" ]; then
  echo "[Makefile]   AVISO: aurora-users exige GLIBC_$need y libs/glibc es $have: puede no cargar en la ISO" >&2
fi
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
