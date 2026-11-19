#!/bin/sh
sleep 30
echo "== con LIBGL_ALWAYS_SOFTWARE=$LIBGL_ALWAYS_SOFTWARE EGL_PLATFORM=$EGL_PLATFORM"
EGL_LOG_LEVEL=debug /usr/bin/egl_dev_test 2>&1 | head -40; echo "rc=$?"
echo "== sin EGL_PLATFORM"
env -u EGL_PLATFORM EGL_LOG_LEVEL=debug /usr/bin/egl_dev_test 2>&1 | head -40
echo "EGL-PROBE: fin"
