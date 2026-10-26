#!/bin/bash
export WAYLAND_DEBUG=1 TK_HEAD=120 TK_TAIL=30 TK_GAP=100 TK_SHOT=5 GDK_DEBUG=misc,frames
exec /usr/bin/toolkits.sh /usr/bin/gtk3-widget-factory
