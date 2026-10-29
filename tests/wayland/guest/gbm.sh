#!/bin/bash
# L7 hito 3: cliente EGL/GBM (aw_gbm_test) sobre renderD128 + linux-dmabuf.
# Uso: gbm.sh [swrast|crocus|iris] [--no-wayland]
#   swrast (por defecto, QEMU): LIBGL_ALWAYS_SOFTWARE=1, llvmpipe via kms_swrast.
#   crocus / iris (Portegé, Haswell = crocus): MESA_LOADER_DRIVER_OVERRIDE; la
#   GPU real exige lkpi.load=linuxkpi,lkpi_drm,lkpi_ttm,lkpi_drm_display,lkpi_i915.
# Resultado en el log serie: lineas "[aw_gbm_test] ..." y "[gbm.sh] RESULT".
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
MODE=${1:-swrast}
shift
# swrast reserva sus buffers con dumb buffers, que Linux solo permite en el
# nodo primario (card0): en un nodo render da EACCES. Un driver de GPU usa renderD128.
case "$MODE" in
  swrast) export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=${GALLIUM_DRIVER:-llvmpipe}; NODE=${NODE:-/dev/dri/card0} ;;
  *) export MESA_LOADER_DRIVER_OVERRIDE=$MODE; NODE=${NODE:-/dev/dri/renderD128} ;;
esac
export MESA_DEBUG=1 LIBGL_DEBUG=verbose EGL_LOG_LEVEL=${EGL_LOG_LEVEL:-debug}
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
sleep 3
timeout ${GBM_TIMEOUT:-120} /usr/bin/aw_gbm_test "$NODE" "$@" --exit >/tmp/gbm.out 2>&1
RC=$?
sleep 2
kill -TERM $APID 2>/dev/null
# Todo lo que escribe el cliente (stdout y stderr de Mesa) tambien al log del kernel:
# en la Portegé la salida de la consola no se guarda, /data/aurora.log si.
grep -a -v 'No DRI config supports' /tmp/gbm.out | head -400 | while IFS= read -r l; do echo "GBMLOG $l" > /dev/kmsg; done
grep -a '^\[aw_gbm_test\]\|libEGL warning\|libEGL error\|^MESA\|crocus\|i915\|INTEL' /tmp/gbm.out | head -80
echo "=== aw.log ==="
grep -a "aurora-wayland\]" /tmp/aw.log | tail -10
echo "[gbm.sh] RESULT rc=$RC mode=$MODE"
echo DRIVE-DONE
