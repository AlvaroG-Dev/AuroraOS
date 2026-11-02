#!/bin/bash
# Compila las herramientas de cuentas (login, su, passwd, useradd, sudo) contra
# la glibc del host (la misma que va en la ISO) y las instala en sysroot/usr/bin.
# su/passwd/sudo/login quedan setuid-root (4755). Sin busybox ni PAM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/user/aurora-users/aurora-users.c"
OUT="$ROOT/sysroot/usr/bin"
mkdir -p "$OUT"
# crypt(): se enlaza la libcrypt.so.1 de libs/glibc (la de la ISO) y no hace
# falta libcrypt-dev ni crypt.h en el host.
GLIBC_CRYPT="$ROOT/libs/glibc/lib/x86_64-linux-gnu/libcrypt.so.1"
[ -f "$GLIBC_CRYPT" ] || GLIBC_CRYPT=$(ls /lib/x86_64-linux-gnu/libcrypt.so.1 /usr/lib/x86_64-linux-gnu/libcrypt.so.1 2>/dev/null | head -1)
[ -f "$GLIBC_CRYPT" ] || { echo "[Makefile]   ERROR: no hay libcrypt.so.1 (ni en libs/glibc ni en el host)" >&2; exit 1; }
gcc -O2 -Wall -Wextra -Wno-unused-result -DAU_NO_CRYPT_H -o "$OUT/aurora-users" "$SRC" "$GLIBC_CRYPT"
# Aviso si el binario exige una glibc mas nueva que la de la ISO (libs/glibc).
need=$(objdump -T "$OUT/aurora-users" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1 || true)
have=$(strings -a "$ROOT/libs/glibc/lib/x86_64-linux-gnu/libc.so.6" 2>/dev/null | sed -n 's/.*release version \([0-9][0-9.]*[0-9]\).*/\1/p' | head -1 || true)
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
cp -f "$OUT/aurora-users" "$OUT/aurora-switch"
chmod 4755 "$OUT/aurora-switch"
cp -f "$OUT/aurora-users" "$OUT/sync"
chmod 0755 "$OUT/sync"

# [U8.2] pamcheck contra libpam.so.0 de la glibc del guest (sin cabeceras PAM).
GLIBC_PAM="$ROOT/libs/glibc/lib/x86_64-linux-gnu/libpam.so.0"
if [ -f "$GLIBC_PAM" ]; then
  gcc -O2 -Wall -o "$OUT/pamcheck" "$ROOT/user/aurora-users/pamcheck.c" "$GLIBC_PAM"
  chmod 0755 "$OUT/pamcheck"
fi
# unix_chkpwd (pam_unix para no-root) necesita setuid root; git no guarda ese bit.
for f in "$ROOT/sysroot/sbin/unix_chkpwd" "$ROOT/sysroot/usr/sbin/unix_chkpwd"; do
  [ -f "$f" ] && chmod 4755 "$f"
done
# Comprobacion final: si falta algo, el build falla en vez de dar una ISO sin
# la herramienta (p. ej. aurora-switch "no se encuentra" en el hardware).
missing=""
for f in aurora-login aurora-su aurora-passwd aurora-sudo aurora-switch aurora-useradd \
         login su passwd sudo useradd sync $( [ -f "$GLIBC_PAM" ] && echo pamcheck ); do
  [ -x "$OUT/$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
  echo "[Makefile]   ERROR: faltan en sysroot/usr/bin:$missing" >&2
  exit 1
fi
echo "[Makefile]   aurora-users: login su passwd sudo aurora-switch (setuid) + useradd + sync + pamcheck: OK"
