#!/bin/bash
# GTK3 minima + Qt5 (ejemplo "Application") como ventanas de Aurora.
export TK_GAP=${TK_GAP:-240} TK_SHOT=5 TK_HEAD=10 TK_TAIL=10
exec /usr/bin/toolkits.sh /usr/bin/qt5-application
