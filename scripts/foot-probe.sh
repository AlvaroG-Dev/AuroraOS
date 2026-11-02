#!/bin/bash
# [U5b] Diagnostico: foot como alvaro bajo aur.session=user:alvaro.
set -u
cd "$(dirname "$0")/.."
export WITH_NVIDIA=0
cat > sysroot/usr/bin/foot-probe.sh <<'PROBE'
sleep 30
su alvaro -c 'export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0; timeout 15 /usr/bin/foot -d info >/tmp/foot.out 2>&1' &
sleep 8
for d in /proc/[0-9]*; do
  echo "FOOTPROC $(grep -h -E '^(Name|Uid|Gid|Groups):' $d/status 2>/dev/null | tr '\n\t' '  ')"
done
ls -ld / /bin /bin/sh /usr/bin/sh | sed 's/^/FOOTLS /'
sleep 9
echo "FOOTOUT $(cat /tmp/foot.out | tr '\n' '|')"
echo FOOTDONE
PROBE
python3 -I scripts/qemu-drive.py -m 2G -t 400 --log /tmp/foot-test.log \
  --cmdline "aur.session=user:alvaro" --done FOOTDONE \
  --boot-test '(bash /usr/bin/foot-probe.sh &)' >/tmp/foot-test.out 2>&1
rm -f sysroot/usr/bin/foot-probe.sh
grep -a "FOOT" /tmp/foot-test.log | cut -c1-260
