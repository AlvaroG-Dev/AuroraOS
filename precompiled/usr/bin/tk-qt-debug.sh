#!/bin/bash
export WAYLAND_DEBUG=1 TK_GAP=200 TK_SHOT=5 TK_HEAD=70 TK_TAIL=40
exec /usr/bin/toolkits.sh /usr/bin/qt5-application
