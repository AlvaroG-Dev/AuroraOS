#!/bin/bash
# N5b: Vulkan sobre la RTX 4060 (ICD de NVIDIA). Uso: vk.sh [--no-wayland] [segundos]
#  Etapa 1 (siempre): GPU escribe un buffer (vkCmdFillBuffer) y la CPU lo lee.
#  Etapa 2 (con Wayland, el escritorio debe estar abierto): swapchain + 3 cuadros de colores.
# Resultado: lineas "VK ..." en el log del kernel (/dev/kmsg) y en la consola.
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export __NV_DISABLE_EXPLICIT_SYNC=${VK_NOSYNC:-1}
export VK_LOADER_DEBUG=${VK_LOADER_DEBUG:-error,warn}
ARGS=
[ "$1" = "--no-wayland" ] && { ARGS=--no-wayland; shift; }
T=${1:-60}
timeout $T /usr/bin/aw_vk_test $ARGS >/tmp/vk.out 2>&1
RC=$?
echo "VK rc=$RC" > /dev/kmsg
echo "VK out: $(grep -a '^\[aw_vk\]' /tmp/vk.out | head -100 | tr '\n' '|')" > /dev/kmsg
echo "VK err: $(sed '/^\[aw_vk\]/d' /tmp/vk.out | head -c 3000 | tr '\n' '|')" > /dev/kmsg
grep -a '^\[aw_vk\]' /tmp/vk.out | head -60
sed '/^\[aw_vk\]/d' /tmp/vk.out | head -15
echo "[vk.sh] RESULT rc=$RC"
