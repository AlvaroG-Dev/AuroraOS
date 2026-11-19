#!/bin/sh
# Prueba de Wine sin pantalla: crea un prefijo y ejecuta cmd.exe (64 bits).
export WINEPREFIX=/tmp/wp HOME=/tmp WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=${WINEDEBUG:-err+all,warn+module,+loaddll,+process,fixme-all}
echo "WINE-PROBE: inicio $(date +%s)"
wine --version
mkdir -p /tmp/sl && ln -sf / /tmp/sl/z: && ln -sf /usr /tmp/sl/u
for f in /tmp/sl/z:/usr/lib/wine/x86_64-windows/kernel32.dll /tmp/sl/z:/usr/bin /tmp/sl/u/lib/wine/x86_64-windows/kernel32.dll /tmp/sl/z:/etc/passwd /tmp/sl/z:/tmp; do
  if [ -e "$f" ]; then echo "SYMLINK-OK $f"; else echo "SYMLINK-FALLA $f"; fi
done
timeout 600 wineboot -i > /tmp/wineboot.log 2>&1; echo "WINE-PROBE: wineboot rc=$?"
echo "WINEBOOT-LOG: $(grep -c . /tmp/wineboot.log) lineas"
grep -v 'map_image_into_view' /tmp/wineboot.log | head -300
ls -la /tmp/wp /tmp/wp/dosdevices /tmp/wp/drive_c 2>&1 | head -30
ls /tmp/wp/drive_c/windows/system32 2>/dev/null | wc -l
timeout 120 wine cmd /c "echo HOLA-DESDE-WINDOWS & ver" 2>&1 | tail -5
echo "WINE-PROBE: fin"
