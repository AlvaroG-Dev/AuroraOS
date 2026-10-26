#!/bin/bash
export STRACE=gtk3-widget-factory TK_GAP=100 TK_SHOT=5 TK_HEAD=5 TK_TAIL=5
exec /usr/bin/toolkits.sh /usr/bin/gtk3-widget-factory
