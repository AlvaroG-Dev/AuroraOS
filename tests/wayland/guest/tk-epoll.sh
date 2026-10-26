#!/bin/bash
# poll sobre epoll + GLib + (con el arreglo) GTK3 widget-factory como ventana de Aurora.
klog() { awk '{ print "EPLOG " substr($0, 1, 200) > "/dev/kmsg"; fflush("/dev/kmsg") }'; }
/usr/bin/epoll_poll_test 2>&1 | klog
export TK_GAP=${TK_GAP:-120} TK_SHOT=5 TK_HEAD=6 TK_TAIL=6
exec /usr/bin/toolkits.sh /usr/bin/gtk3-widget-factory
