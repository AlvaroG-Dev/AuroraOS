#!/bin/bash
# Drivers de pantalla .aur (virtio_gpu.aur / bochs.aur) y DRM: el host captura
# la pantalla de QEMU en cada marcador SHOT-*.
sleep ${GPU_WAIT:-30}
echo "[gpu.sh] SHOT-1"
sleep 10
ls -la /dev/gpuproxy
echo "[gpu.sh] drm_info:"
/usr/bin/drm_info 2>&1 | grep -a -i "driver\|EDID\|edid\|Virtual\|desc" | head -12 | while IFS= read -r l; do echo "GPULOG $l" > /dev/kmsg; sleep 0.05; done
echo "[gpu.sh] SHOT-2"
sleep 8
echo DRIVE-DONE
