#!/bin/bash
# [U7.4] Cambio de sesion: alvaro -> root -> alvaro con aurora-switch.
set -u
cd "$(dirname "$0")/.."
export WITH_NVIDIA=0
rm -f kernel/initrd.tar
cat > sysroot/usr/bin/switch-probe.sh <<'PROBE'
snap() {
  for d in /proc/[0-9]*; do
    n=$(grep -h '^Name:' $d/status 2>/dev/null | cut -f2)
    case "$n" in aurora-wayland|terminal|sh|Xwayland) echo "SW $1 $n $(grep -h '^Uid:' $d/status | cut -f2)";; esac
  done
  echo "SW $1 session=$(cat /run/aurora-session)"
}
sleep 30; snap A1
aurora-switch root; sleep 20; snap B_root
aurora-switch alvaro; sleep 20; snap C_alvaro
echo SWDONE
PROBE
python3 -I scripts/qemu-drive.py -m 2G -t 420 --log /tmp/switch-test.log \
  --cmdline "aur.session=user:alvaro" --done SWDONE \
  --boot-test '(bash /usr/bin/switch-probe.sh &)' >/tmp/switch-test.out 2>&1
rm -f sysroot/usr/bin/switch-probe.sh kernel/initrd.tar
grep -a "^SW \|init: cambio\|AUDIT\] switch" /tmp/switch-test.log | cut -c1-150
