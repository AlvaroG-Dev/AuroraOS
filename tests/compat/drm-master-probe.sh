#!/bin/sh
sleep 60
ls -la /dev/dri
/usr/bin/drm_master_test
sleep 12
echo "DRM-PROBE: fin"
