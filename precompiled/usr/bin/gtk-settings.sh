#!/bin/bash
# Comprueba que GTK3 lee /usr/share/aurora/xdg/gtk-3.0/settings.ini
export STRACE=hello-gtk TK_GAP=60 TK_SHOT=3 TK_HEAD=3 TK_TAIL=3
echo "GSLOG XDG_CONFIG_DIRS=$XDG_CONFIG_DIRS HOME=$HOME" > /dev/kmsg
head -c 60 /usr/share/aurora/xdg/gtk-3.0/settings.ini | head -2 | while IFS= read -r l; do echo "GSLOG $l" > /dev/kmsg; done
exec /usr/bin/toolkits.sh /usr/bin/hello-gtk
