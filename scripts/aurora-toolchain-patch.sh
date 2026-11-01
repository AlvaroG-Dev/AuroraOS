#!/bin/bash
# scripts/aurora-toolchain-patch.sh — Parchea fuentes de binutils 2.42 y GCC 13.3
# para el triplete x86_64-pc-aurora-gnu (Plan_GCC_Autocompilacion.md §2.2).
# Idempotente. Uso: scripts/aurora-toolchain-patch.sh <binutils-dir> <gcc-dir>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BU=${1:?binutils dir}; GCC=${2:?gcc dir}
rep() {  # rep fichero 'viejo' 'nuevo'  (literal, debe existir una vez como minimo)
    python3 -I - "$@" <<'PY'
import sys
f, old, new = sys.argv[1:4]
s = open(f).read()
if new in s: sys.exit(0)
if old not in s: sys.exit("no encontrado en %s: %r" % (f, old))
open(f, "w").write(s.replace(old, new, 1))
PY
}
repall() { # como rep pero todas las apariciones
    python3 -I - "$@" <<'PY'
import sys
f, old, new = sys.argv[1:4]
s = open(f).read()
if old not in s: sys.exit(0)
if new in s and old != new: pass
open(f, "w").write(s.replace(old, new))
PY
}

# --- config.sub / config.guess en ambos arboles ------------------------------
"$ROOT/scripts/aurora-config-sub.sh" "$BU" >/dev/null
"$ROOT/scripts/aurora-config-sub.sh" "$GCC" >/dev/null

# --- binutils ------------------------------------------------------------------
rep "$BU/bfd/config.bfd" '  x86_64-*-linux-*)' '  x86_64-*-linux-* | x86_64-*-aurora*)'
rep "$BU/gas/configure.tgt" '  i386-*-linux-*)			fmt=elf em=linux' '  i386-*-linux-* | i386-*-aurora*)			fmt=elf em=linux'
rep "$BU/ld/configure.tgt" 'x86_64-*-linux-*)	targ_emul=elf_x86_64' 'x86_64-*-linux-* | x86_64-*-aurora*)	targ_emul=elf_x86_64'
rep "$BU/ld/configure.tgt" 'i[3-7]86-*-linux-* | x86_64-*-linux-*)' 'i[3-7]86-*-linux-* | x86_64-*-linux-* | x86_64-*-aurora*)'

# --- GCC: ficheros de configuracion de destino ------------------------------------
cp -f "$ROOT/toolchain-aurora/aurora.h" "$GCC/gcc/config/aurora.h"
G="$GCC/gcc/config.gcc"
rep "$G" '*-*-linux* | frv-*-*linux* | *-*-kfreebsd*-gnu | *-*-gnu* | *-*-kopensolaris*-gnu | *-*-uclinuxfdpiceabi)
  extra_options' '*-*-linux* | *-*-aurora* | frv-*-*linux* | *-*-kfreebsd*-gnu | *-*-gnu* | *-*-kopensolaris*-gnu | *-*-uclinuxfdpiceabi)
  extra_options'
rep "$G" '    *-*-linux* | frv-*-*linux* | *-*-kfreebsd*-gnu | *-*-kopensolaris*-gnu)
      :;;' '    *-*-linux* | *-*-aurora* | frv-*-*linux* | *-*-kfreebsd*-gnu | *-*-kopensolaris*-gnu)
      :;;'
rep "$G" '    *linux*)
      tm_p_file="${tm_p_file} linux-protos.h"' '    *linux* | *-*-aurora*)
      tm_p_file="${tm_p_file} linux-protos.h"'
rep "$G" '    *-*-*linux*)
      d_target_objs="${d_target_objs} linux-d.o"' '    *-*-*linux* | *-*-aurora*)
      d_target_objs="${d_target_objs} linux-d.o"'
rep "$G" 'x86_64-*-linux* | x86_64-*-kfreebsd*-gnu | x86_64-*-gnu*)
	tm_file="${tm_file} i386/unix.h' 'x86_64-*-linux* | x86_64-*-aurora* | x86_64-*-kfreebsd*-gnu | x86_64-*-gnu*)
	tm_file="${tm_file} i386/unix.h'
rep "$G" '	x86_64-*-linux*)
		tm_file="${tm_file} linux.h linux-android.h i386/linux-common.h i386/linux64.h"' '	x86_64-*-aurora*)
		tm_file="${tm_file} linux.h linux-android.h i386/linux-common.h i386/linux64.h aurora.h"
		extra_options="${extra_options} linux-android.opt"
		;;
	x86_64-*-linux*)
		tm_file="${tm_file} linux.h linux-android.h i386/linux-common.h i386/linux64.h"'
repall "$G" 'i[34567]86-*-linux* | x86_64-*-linux*)' 'i[34567]86-*-linux* | x86_64-*-linux* | x86_64-*-aurora*)'
rep "$GCC/libgcc/config.host" '*-*-linux* | frv-*-*linux* | *-*-kfreebsd*-gnu' '*-*-linux* | *-*-aurora* | frv-*-*linux* | *-*-kfreebsd*-gnu'
rep "$GCC/libgcc/config.host" 'x86_64-*-linux*)' 'x86_64-*-linux* | x86_64-*-aurora*)'
rep "$GCC/libstdc++-v3/configure.host" '  gnu* | linux* | kfreebsd*-gnu | uclinux*)' '  gnu* | linux* | aurora* | kfreebsd*-gnu | uclinux*)'
rep "$GCC/libstdc++-v3/configure.host" '  *-*-linux* | *-*-uclinux*)' '  *-*-linux* | *-*-aurora* | *-*-uclinux*)'

# --- GCC: scripts configure generados (libtool, crossconfig, comprobaciones) ---
# Se añade "aurora*" junto a cada etiqueta "linux*" (host_os) y "*-*-aurora*"
# junto a "*-*-linux*" (target) en las etiquetas de case. Solo en los
# directorios que se configuran para el destino y en el propio gcc/.
for d in gcc libgcc libstdc++-v3 libgomp libatomic libssp libquadmath libitm libbacktrace lto-plugin libcc1 libdecnumber libcpp libiberty intl zlib fixincludes .; do
    for f in "$GCC/$d/configure" "$GCC/$d/aclocal.m4" "$GCC/$d/libtool.m4" "$GCC/$d/crossconfig.m4" "$GCC/$d/configure.tgt"; do
        [ -f "$f" ] || continue
        grep -q 'AURORA-PATCHED' "$f" 2>/dev/null && continue
        python3 -I - "$f" <<'PY'
import re, sys
f = sys.argv[1]
s = open(f).read()
# etiquetas de case que empiezan por 'linux*' (host_os) -> 'aurora* | linux*'
s2 = re.sub(r'(?m)^(\s*)(linux\*)(\s*[|)])', r'\1aurora* | \2\3', s)
s2 = re.sub(r'(?m)^(\s*)(\*-\*-linux\*)(\s*[|)])', r'\1*-*-aurora* | \2\3', s2)
s2 = re.sub(r'(?m)^(\s*)(gnu\* \| linux\*)', r'\1gnu* | aurora* | linux*', s2)
s2 = s2.replace('| linux* |', '| aurora* | linux* |') if False else s2
if s2 != s:
    open(f, "w").write(s2)
PY
    done
done
echo "[aurora-patch] binutils y GCC parcheados para x86_64-pc-aurora-gnu"
