#!/bin/bash
# N5a: cliente Wayland con EGL propietario de NVIDIA (egl-wayland) sobre aurora-wayland.
# Uso: nvwl.sh [segundos] [cliente]   (por defecto aw_nvwl_test, diagnostico paso a paso; tambien weston-simple-egl)
# Resultado: lineas "NVWL ..." en el log del kernel (/dev/kmsg) y en la consola.
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
export GBM_BACKENDS_PATH=/usr/lib/x86_64-linux-gnu/gbm
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export EGL_PLATFORM=wayland EGL_LOG_LEVEL=debug WAYLAND_DEBUG=${WAYLAND_DEBUG:-0}
T=${1:-8}; CLI=${2:-aw_nvwl_test}
if ! ls $XDG_RUNTIME_DIR/wayland-0 >/dev/null 2>&1; then
  /usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
  APID=$!; sleep 3
fi
run_one() { # nombre json extra-env
  NAME=$1; export __EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES=$2
  [ -n "$3" ] && export $3
  timeout $T $CLI >/tmp/nvwl-$NAME.out 2>&1
  RC=$?
  echo "NVWL[$NAME] rc=$RC (124 = siguio dibujando hasta el timeout = OK)" > /dev/kmsg
  echo "NVWL[$NAME] out: $(grep -a '^\[aw_nvwl\]' /tmp/nvwl-$NAME.out | grep -av 'mod\[' | head -100 | tr '\n' '|')" > /dev/kmsg
  echo "NVWL[$NAME] err: $(grep -av '^\[aw_nvwl\]' /tmp/nvwl-$NAME.out | head -c 1500 | tr '\n' '|')" > /dev/kmsg
  echo "=== $NAME rc=$RC ==="
  grep -a '^\[aw_nvwl\]' /tmp/nvwl-$NAME.out | grep -av 'mod\[' | head -40
  grep -av '^\[aw_nvwl\]' /tmp/nvwl-$NAME.out | head -5
}
# Con NVWL_PLATFORM se prueba solo esa; sin el, las dos: egl-wayland (1.x) y egl-wayland2 sin explicit sync.
if [ -n "$NVWL_PLATFORM" ]; then
  run_one custom "$NVWL_PLATFORM" ""
else
  run_one wayland1 /usr/share/egl/egl_external_platform.d/10_nvidia_wayland.json ""
  run_one wayland2 /usr/share/egl/egl_external_platform.d/09_nvidia_wayland2.json __NV_DISABLE_EXPLICIT_SYNC=1
fi
[ -n "$APID" ] && kill -TERM $APID 2>/dev/null
echo "[nvwl.sh] RESULT"
