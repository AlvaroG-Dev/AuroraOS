#!/bin/bash
# Diagnostico de Xwayland en Aurora: ejecuta xkbcomp como lo hace Xwayland.
echo "[xkb-test] inicio"
for d in /tmp /run/user/0 /usr/share/X11/xkb/compiled /var/lib/xkb; do
  if [ -d "$d" ]; then
    touch "$d/.t" 2>/dev/null && { echo "[xkb-test] $d escribible"; rm -f "$d/.t"; } || echo "[xkb-test] $d NO escribible"
  else
    echo "[xkb-test] $d no existe"
  fi
done
cat > /tmp/km.txt <<'KM'
xkb_keymap { xkb_keycodes { include "evdev+aliases(qwerty)" }; xkb_types { include "complete" }; xkb_compat { include "complete" }; xkb_symbols { include "pc+us" }; xkb_geometry { include "pc(pc105)" }; };
KM
sh -c '"/usr/bin/xkbcomp" -w 1 "-R/usr/share/X11/xkb" -xkm "-" -em1 "m1" -emp "> " -eml "m3" "/tmp/server-t.xkm"' < /tmp/km.txt
echo "[xkb-test] xkbcomp rc=$?"
ls -la /tmp/server-t.xkm 2>&1
echo "[xkb-test] fin"
