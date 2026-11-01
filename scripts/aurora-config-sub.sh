#!/bin/bash
# scripts/aurora-config-sub.sh — Hace que config.sub/config.guess de un arbol
# de fuentes conozcan el triplete de AuroraOS: x86_64-pc-aurora-gnu.
# (Plan_GCC_Autocompilacion.md §2.2). No toca nada mas del paquete.
#
# Uso:
#   scripts/aurora-config-sub.sh <dir-fuentes>   # parchea todos los config.sub/guess
#   scripts/aurora-config-sub.sh --selftest      # prueba con el config.sub del host
#
# Idempotente: si ya esta parcheado no hace nada.
set -euo pipefail

patch_sub() {
    local f=$1
    grep -q 'aurora\*-gnu\*' "$f" && return 0
    python3 -I - "$f" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
# 1) lista de pares KERNEL-OS de tres campos
t = "| uclinux-gnu* | kfreebsd*-gnu* |"
if t not in s:
    sys.exit("config.sub inesperado (version no soportada): " + p)
s = s.replace(t, "| uclinux-gnu* | aurora*-gnu* | kfreebsd*-gnu* |", 1)
# 2) validacion KERNEL-OS (con o sin '-' final segun la version)
m = re.search(r"(?m)^(\t)(kfreebsd\*-gnu\*(-?) \| kopensolaris\*-gnu\*-?\))", s)
if not m:
    sys.exit("config.sub inesperado (validacion KERNEL-OS): " + p)
suf = m.group(3)
s = s[:m.start()] + "\taurora*-gnu*" + suf + " | " + s[m.start()+1:]
open(p, "w").write(s)
PY
}

patch_guess() {
    local f=$1
    grep -q 'x86_64-pc-aurora-gnu' "$f" && return 0
    python3 -I - "$f" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
m = "    aarch64:Linux:*:*)"
if m not in s:
    sys.exit("config.guess inesperado (version no soportada): " + p)
add = "    x86_64:Aurora:*:*)\n\tGUESS=x86_64-pc-aurora-gnu\n\t;;\n"
s = s.replace(m, add + m, 1)
open(p, "w").write(s)
PY
}

if [ "${1:-}" = "--selftest" ]; then
    t=$(mktemp -d)
    cp /usr/share/misc/config.sub /usr/share/misc/config.guess "$t/"
    patch_sub "$t/config.sub"; patch_guess "$t/config.guess"
    r=$(sh "$t/config.sub" x86_64-pc-aurora-gnu)
    [ "$r" = "x86_64-pc-aurora-gnu" ] || { echo "FALLO config.sub: $r"; exit 1; }
    sh "$t/config.sub" x86_64-pc-linux-gnu >/dev/null || { echo "FALLO: linux-gnu roto"; exit 1; }
    sh "$t/config.sub" x86_64-aurora-gnu | grep -q aurora || { echo "FALLO forma corta"; exit 1; }
    # config.guess: simular uname -s = Aurora con un uname falso
    mkdir "$t/bin"; printf '#!/bin/sh\ncase "$1" in -s) echo Aurora;; -m) echo x86_64;; -r) echo 0.1;; -v) echo v;; -n) echo a;; *) echo Aurora;; esac\n' > "$t/bin/uname"; chmod +x "$t/bin/uname"
    g=$(PATH="$t/bin:$PATH" sh "$t/config.guess")
    [ "$g" = "x86_64-pc-aurora-gnu" ] || { echo "FALLO config.guess: $g"; exit 1; }
    echo "selftest OK: sub=$r guess=$g"; rm -rf "$t"; exit 0
fi

dir=${1:?uso: $0 <dir-fuentes> | --selftest}
n=0
while IFS= read -r f; do
    case $(basename "$f") in
        config.sub)   patch_sub "$f";   n=$((n+1)) ;;
        config.guess) patch_guess "$f"; n=$((n+1)) ;;
    esac
done < <(find "$dir" -type f \( -name config.sub -o -name config.guess \))
echo "[aurora-config] $n ficheros revisados en $dir"
