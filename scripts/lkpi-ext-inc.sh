#!/bin/sh
# Vista de las cabeceras de interfaz del arbol de Linux que la capa usa tal
# cual (UAPI de virtio, include/drm y uapi/drm), sin copiarlas al repo:
# build/linuxkpi/ext-inc/ con enlaces simbolicos al arbol descargado
# (scripts/fetch-linux-drivers.sh). Va al final de la ruta de includes, asi que
# las cabeceras propias de include/linuxkpi mandan siempre.
#   linux/<f>          UAPI sin cabecera propia del mismo nombre (virtio_ids.h ...)
#   uapi/linux/<f>     toda la UAPI de virtio (virtio_ring.h, virtio_config.h ...)
#   drm/<f>            include/drm/* y uapi/drm/* (drm_fourcc.h, drm_mode.h ...)
#   linux/hdmi.h, media-bus-format.h, cec*.h, video/*  (declaraciones de HDMI y video)
set -e
cd "$(dirname "$0")/.."
SRC=${LKPI_LINUX:-build/linuxkpi_src/linux}
OUT=build/linuxkpi/ext-inc
[ -d "$SRC/include/uapi" ] || exit 0
STAMP="$OUT/.stamp"
WANT="$(cat "$SRC/.aurora-version" 2>/dev/null || echo none)+net12"
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
	mkdir -p "$OUT/uapi/drm"
	# kernel primero (como en Linux, include/ antes que include/uapi/), UAPI despues
	for f in "$ABS"/include/drm/*.h; do
		[ -e "$f" ] && ln -sf "$f" "$OUT/drm/$(basename "$f")"
	done
	for d in "$ABS"/include/drm/*/; do
		[ -d "$d" ] && ln -sfn "${d%/}" "$OUT/drm/$(basename "$d")"
	done
	for f in "$ABS"/include/uapi/drm/*.h; do
		b=$(basename "$f")
		ln -sf "$f" "$OUT/uapi/drm/$b"
		[ -e "$OUT/drm/$b" ] || ln -sf "$f" "$OUT/drm/$b"
	done
fi
# Cabeceras de subsistemas que DRM usa tal cual (solo declaraciones): HDMI,
# videomode/display_timing/cmdline y los formatos de bus de media.
for h in hdmi.h aperture.h screen_info.h; do
	[ -e "$ABS/include/linux/$h" ] && [ ! -e "include/linuxkpi/linux/$h" ] && ln -sf "$ABS/include/linux/$h" "$OUT/linux/$h"
done
for h in media-bus-format.h cec.h cec-funcs.h fb.h vesa.h screen_info.h; do
	[ -e "$ABS/include/uapi/linux/$h" ] || continue
	ln -sf "$ABS/include/uapi/linux/$h" "$OUT/uapi/linux/$h"
	[ -e "include/linuxkpi/linux/$h" ] || ln -sf "$ABS/include/uapi/linux/$h" "$OUT/linux/$h"
done
# Red (Plan_Red.md): cabeceras de Ethernet/MII/ethtool/phylib/IP del arbol tal
# cual (declaraciones). Las propias de include/linuxkpi/linux mandan siempre;
# estas solo rellenan lo que no tiene version propia (kernel primero, UAPI despues).
mkdir -p "$OUT/net" "$OUT/kreal"
ln -sf "$ABS/include/linux/ethtool.h" "$OUT/kreal/ethtool.h"
for f in "$ABS"/include/uapi/linux/*.h; do [ -e "$OUT/uapi/linux/$(basename "$f")" ] || ln -sf "$f" "$OUT/uapi/linux/$(basename "$f")"; done
ln -sfn "$ABS/include/uapi/linux/hdlc" "$OUT/linux/hdlc"
for h in if.h libc-compat.h; do [ -e "include/linuxkpi/linux/$h" ] || [ -e "$OUT/linux/$h" ] || ln -sf "$ABS/include/uapi/linux/$h" "$OUT/linux/$h"; done
for h in if_ether.h netlink.h; do ln -sf "$ABS/include/uapi/linux/$h" "$OUT/uapi/linux/$h"; done
for h in ethtool.h mii.h mdio.h phy.h linkmode.h mii_timestamper.h \
         if_link.h ethtool_netlink.h net_tstamp.h \
         phylib_stubs.h in6.h icmp.h; do
	if [ -e "$ABS/include/uapi/linux/$h" ]; then
		ln -sf "$ABS/include/uapi/linux/$h" "$OUT/uapi/linux/$h"
	fi
	if [ -e "include/linuxkpi/linux/$h" ]; then continue; fi
	if [ -e "$ABS/include/linux/$h" ]; then ln -sf "$ABS/include/linux/$h" "$OUT/linux/$h"
	elif [ -e "$ABS/include/uapi/linux/$h" ]; then ln -sf "$ABS/include/uapi/linux/$h" "$OUT/linux/$h"; fi
done
for h in eee.h; do
	[ -e "$ABS/include/net/$h" ] && ln -sf "$ABS/include/net/$h" "$OUT/net/$h"
done
mkdir -p "$OUT/linux/platform_data"
[ -e "$ABS/include/linux/platform_data/simplefb.h" ] && ln -sf "$ABS/include/linux/platform_data/simplefb.h" "$OUT/linux/platform_data/simplefb.h"
[ -d "$ABS/include/video" ] && ln -sfn "$ABS/include/video" "$OUT/video"
echo "$WANT" > "$STAMP"
