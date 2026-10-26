#!/bin/bash
# D3 + D8:
#  - PRIMARY X11 <-> seleccion primaria de Wayland: wl-copy -p -> xclip -o -selection primary
#    y xclip -i -selection primary -> wl-paste -p.
#  - Icono simbolico de respaldo: un lanzador cuyo unico icono es NOMBRE-symbolic
#    (Adwaita help-contents-symbolic) debe aparecer con icono en el Dock.
# El host captura la pantalla en SHOT-1.
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export DISPLAY=:0
export AW_DEBUG=1
export AW_APPS_DIR=/tmp/apps
mkdir -p /tmp/apps
cp /usr/share/applications/*.desktop /tmp/apps/ 2>/dev/null
cat > /tmp/apps/ayuda-simbolica.desktop <<'EOT'
[Desktop Entry]
Type=Application
Name=Ayuda
Exec=/usr/bin/ayuda-demo
Icon=help-contents
StartupWMClass=ayuda
EOT
echo "[primary-icons.sh] desktop: $(ls -l /tmp/apps/ayuda-simbolica.desktop 2>&1)"
/usr/bin/aurora-wayland >/tmp/aw.log 2>&1 &
APID=$!
for _ in $(seq 1 60); do
  [ -e /tmp/.X11-unix/X0 ] && break
  sleep 2
done
sleep 5
/usr/bin/xclock >/dev/null 2>&1 &
/usr/bin/xterm -geometry 40x6 -e /bin/sh -c 'sleep 600' >/dev/null 2>&1 &
sleep ${CX_WAIT:-40}

echo -n "prim-de-wayland-123" | /usr/bin/wl-copy -p >/tmp/copy1.out 2>&1
sleep 3
R1=$(timeout 20 /usr/bin/xclip -o -selection primary 2>/tmp/x1.err)
echo "[primary-icons.sh] PRIMARY WAYLAND->X11 xclip -o = '$R1' (esperado 'prim-de-wayland-123') $(cat /tmp/x1.err)"

echo -n "prim-de-x11-456" | /usr/bin/xclip -i -selection primary >/tmp/x2.out 2>&1
sleep 3
R2=$(timeout 20 /usr/bin/wl-paste -p -n 2>/tmp/p2.err)
echo "[primary-icons.sh] PRIMARY X11->WAYLAND wl-paste -p = '$R2' (esperado 'prim-de-x11-456') $(cat /tmp/p2.err)"

# CLIPBOARD no debe mezclarse con PRIMARY
echo -n "solo-clipboard" | /usr/bin/wl-copy >/dev/null 2>&1
sleep 3
R3=$(timeout 20 /usr/bin/xclip -o -selection primary 2>/dev/null)
R4=$(timeout 20 /usr/bin/xclip -o -selection clipboard 2>/dev/null)
echo "[primary-icons.sh] SEPARADOS primary='$R3' clipboard='$R4' (esperado primary=x11-456 clipboard=solo-clipboard)"

ls /tmp/aurora-icons 2>&1 | head -20
echo "[primary-icons.sh] SHOT-1"
sleep ${CX_HOLD:-25}
kill -TERM $APID 2>/dev/null
sleep 2
echo "=== aw.log"; grep -a -e seleccion -e set_selection -e icon -e "X11 0x" /tmp/aw.log | head -60
echo DRIVE-DONE
