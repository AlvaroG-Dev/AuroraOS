#!/bin/bash
# Bucle principal de GLib: timeouts, giro en reposo, hilos que despiertan el contexto.
klog() { awk '{ print "GLLOG " substr($0, 1, 200) > "/dev/kmsg"; fflush("/dev/kmsg") }'; }
/usr/bin/glib_test 2>&1 | klog
echo DRIVE-DONE
