#!/bin/bash
# GTK3 widget-factory con tiempo de sobra (sin KVM tarda en pintar).
export TK_GAP=${TK_GAP:-300} TK_SHOT=5 TK_HEAD=8 TK_TAIL=8
exec /usr/bin/toolkits.sh /usr/bin/gtk3-widget-factory
