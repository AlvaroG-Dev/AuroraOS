#!/bin/bash
# scripts/nvidia-conftest.sh — Plan_NVIDIA.md N2: genera los conftest/*.h de
# kernel-open/ de NVIDIA contra las cabeceras de LinuxKPI (en vez de las de un
# kernel de Linux real), con el propio conftest.sh de NVIDIA. Salida:
# build/nvidia-kpi/out/conftest/{headers,functions,generic,macros,symbols,types,patches}.h
# Uso: scripts/nvidia-conftest.sh [--clean]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
K="$ROOT/build/linuxkpi_src/nvidia-open/kernel-open"
B="$ROOT/build/nvidia-kpi"
SRC="$B/src"; OUT="$B/out"
[ -f "$K/conftest.sh" ] || { echo "faltan las fuentes de NVIDIA (scripts/fetch-linux-drivers.sh)" >&2; exit 1; }
[ "${1:-}" = "--clean" ] && rm -rf "$B"
mkdir -p "$SRC/arch/x86/include" "$OUT/include/linux" "$OUT/include/generated" "$OUT/conftest/compile-tests" "$OUT/conftest/header_presence"
ln -sfn "$ROOT/include/linuxkpi" "$SRC/include"
: > "$OUT/include/linux/autoconf.h"
echo '#define UTS_RELEASE "6.18.59-aurora"' > "$OUT/include/generated/utsrelease.h"
echo "6.18.59-aurora" > "$OUT/conftest/uts_release"

CONF="sh $K/conftest.sh gcc x86_64 $SRC $OUT"
# Banderas: las de conftest + las de la capa (kconfig/compiler_types y las
# cabeceras de interfaz de Linux), sin SSE como un modulo de Linux x86-64.
CF="$($CONF build_cflags) -include linux/kconfig.h -include linux/compiler_types.h -idirafter $ROOT/build/linuxkpi/ext-inc -I$OUT -std=gnu11 -fno-pie -fshort-wchar -fms-extensions -mno-sse -mno-sse2 -mno-mmx -mno-80387 -Wno-error"

# Listas de pruebas (de los .Kbuild de cada modulo y de header-presence-tests.mk)
HDRS=$(sed -n 's/^NV_HEADER_PRESENCE_TESTS *+\?= *//p;s/^ *\([a-z0-9_/.+-]*\.h\) *\\\?$/\1/p' "$K/header-presence-tests.mk" | tr -d '\\' | tr -s ' \n' '\n' | grep '\.h$' | sort -u)
list() { grep -hE "^NV_CONFTEST_$1_COMPILE_TESTS *\+=" "$K"/*/*.Kbuild "$K/Kbuild" 2>/dev/null | sed 's/.*+= *//' | sort -u; }

echo "[conftest] cabeceras: $(echo "$HDRS" | wc -l)"
for h in $HDRS; do
    $CONF test_kernel_header "$CF" "$h" > "$OUT/conftest/header_presence/$(echo "$h" | tr / _).part"
done
cat "$OUT"/conftest/header_presence/*.part > "$OUT/conftest/headers.h"
$CONF patch_check > "$OUT/conftest/patches.h"

for kind in FUNCTION GENERIC MACRO SYMBOL TYPE; do
    lc=$(echo "$kind" | tr 'A-Z' 'a-z')
    s=${lc}s; [ "$kind" = GENERIC ] && s=generic
    n=0; : > "$OUT/conftest/$s.h"
    for t in $(list "$kind"); do
        $CONF compile_tests "$CF" "$t" >> "$OUT/conftest/$s.h" 2>/dev/null || echo "[conftest] FALLA $t" >&2
        n=$((n+1))
    done
    echo "[conftest] $s.h: $n pruebas"
done
# Aurora no tiene memory hotplug (solo Grace/Hopper lo usa): sin onlining NUMA de la VRAM.
sed -i -E 's/^#define (NV_ADD_MEMORY_DRIVER_MANAGED_PRESENT|NV_OFFLINE_AND_REMOVE_MEMORY_PRESENT)/#undef \1/' "$OUT/conftest/functions.h"
echo "[conftest] listo: $OUT/conftest"
