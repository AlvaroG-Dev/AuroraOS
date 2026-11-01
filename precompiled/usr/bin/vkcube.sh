#!/bin/bash
# N5b: cubo Vulkan giratorio (RTX 4060). Uso: vkcube.sh [segundos=15] [fps_max=0]
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export __NV_DISABLE_EXPLICIT_SYNC=${VK_NOSYNC:-1}
export VK_LOADER_DEBUG=${VK_LOADER_DEBUG:-error,warn}
S=${1:-15}
timeout $((S+40)) /usr/bin/aw_vkcube $S ${2:-0} >/tmp/vkcube.out 2>&1
RC=$?
echo "VKCUBE rc=$RC" > /dev/kmsg
echo "VKCUBE out: $(grep -a '^\[aw_vkcube\]' /tmp/vkcube.out | head -100 | tr '\n' '|')" > /dev/kmsg
echo "VKCUBE err: $(sed '/^\[aw_vkcube\]/d' /tmp/vkcube.out | head -c 3000 | tr '\n' '|')" > /dev/kmsg
grep -a '^\[aw_vkcube\]' /tmp/vkcube.out | head -60
sed '/^\[aw_vkcube\]/d' /tmp/vkcube.out | head -15
echo "rc=$RC"
