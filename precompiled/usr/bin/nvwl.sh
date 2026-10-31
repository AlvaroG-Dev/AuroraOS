#!/bin/bash
# N5a: cliente Wayland con EGL propietario de NVIDIA (egl-wayland) sobre aurora-wayland.
# Uso: nvwl.sh [segundos] [cliente]   (por defecto weston-simple-egl 8 s)
# Resultado: lineas "NVWL ..." en el log del kernel (/dev/kmsg) y en la consola.
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
export __EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES=${NVWL_PLATFORM:-/usr/share/egl/egl_external_platform.d/10_nvidia_wayland.json}
export GBM_BACKENDS_PATH=/usr/lib/x86_64-linux-gnu/gbm
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export EGL_PLATFORM=wayland EGL_LOG_LEVEL=debug WAYLAND_DEBUG=${WAYLAND_DEBUG:-0}
T=${1:-8}; CLI=${2:-weston-simple-egl}
if ! ls $XDG_RUNTIME_DIR/wayland-0 >/dev/null 2>&1; then
  /usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
  APID=$!; sleep 3
fi
timeout $T $CLI >/tmp/nvwl.out 2>&1
RC=$?
[ -n "$APID" ] && kill -TERM $APID 2>/dev/null
echo "NVWL rc=$RC (124 = siguio dibujando hasta el timeout = OK)" > /dev/kmsg
echo "NVWL out: $(head -c 1500 /tmp/nvwl.out | tr '\n' '|')" > /dev/kmsg
head -40 /tmp/nvwl.out
echo "[nvwl.sh] RESULT rc=$RC"
