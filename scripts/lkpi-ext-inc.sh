#!/bin/sh
# Vista de las cabeceras de interfaz del arbol de Linux que la capa usa tal
# cual (UAPI de virtio, include/drm y uapi/drm), sin copiarlas al repo:
# build/linuxkpi/ext-inc/ con enlaces simbolicos al arbol descargado
# (scripts/fetch-linux-drivers.sh). Va al final de la ruta de includes, asi que
# las cabeceras propias de include/linuxkpi mandan siempre.
#   linux/<f>          UAPI sin cabecera propia del mismo nombre (virtio_ids.h ...)
#   uapi/linux/<f>     toda la UAPI de virtio (virtio_ring.h, virtio_config.h ...)
#   drm/<f>            include/drm/* y uapi/drm/* (drm_fourcc.h, drm_mode.h ...)
set -e
cd "$(dirname "$0")/.."
SRC=${LKPI_LINUX:-build/linuxkpi_src/linux}
OUT=build/linuxkpi/ext-inc
[ -d "$SRC/include/uapi" ] || exit 0
STAMP="$OUT/.stamp"
WANT=$(cat "$SRC/.aurora-version" 2>/dev/null || echo none)
[ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$WANT" ] && exit 0
rm -rf "$OUT"
mkdir -p "$OUT/linux" "$OUT/uapi/linux" "$OUT/drm"
ABS=$(cd "$SRC" && pwd)
for f in "$ABS"/include/uapi/linux/virtio_*.h; do
	b=$(basename "$f")
	ln -sf "$f" "$OUT/uapi/linux/$b"
	[ -e "include/linuxkpi/linux/$b" ] || ln -sf "$f" "$OUT/linux/$b"
done
if [ -d "$ABS/include/drm" ]; then
	for f in "$ABS"/include/drm/*.h "$ABS"/include/uapi/drm/*.h; do
		[ -e "$f" ] && ln -sf "$f" "$OUT/drm/$(basename "$f")"
	done
	for d in "$ABS"/include/drm/*/; do
		[ -d "$d" ] && ln -sfn "${d%/}" "$OUT/drm/$(basename "$d")"
	done
fi
echo "$WANT" > "$STAMP"
