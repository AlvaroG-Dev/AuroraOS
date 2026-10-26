#!/bin/bash
export STRACE=qt5-application TK_GAP=150 TK_SHOT=5 TK_HEAD=3 TK_TAIL=3
exec /usr/bin/toolkits.sh /usr/bin/qt5-application
