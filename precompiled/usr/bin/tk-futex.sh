#!/bin/bash
# futex_test (WAKE_OP/CMP_REQUEUE) y luego Qt5 y GTK3 reales como ventanas de Aurora.
klog() { awk '{ print "FXLOG " substr($0, 1, 200) > "/dev/kmsg"; fflush("/dev/kmsg") }'; }
/usr/bin/futex_test 2>&1 | klog
export TK_GAP=${TK_GAP:-150} TK_SHOT=5 TK_HEAD=6 TK_TAIL=6
exec /usr/bin/toolkits.sh /usr/bin/qt5-application /usr/bin/gtk3-widget-factory
