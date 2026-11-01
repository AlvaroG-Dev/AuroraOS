#!/bin/bash
# [U5b] aur.session=user:alvaro: el compositor debe correr como uid 1000 y su
# socket vivir en /run/user/1000. Uso: scripts/session-test.sh  (WITH_NVIDIA=0)
set -u
cd "$(dirname "$0")/.."
export WITH_NVIDIA=0
cat > sysroot/usr/bin/session-probe.sh <<'PROBE'
sleep 40
for d in /proc/[0-9]*; do
  echo "SESSPROC $(grep -h -E '^(Name|Uid):' $d/status 2>/dev/null | tr '\n\t' '  ')"
done
ls -ln /run/user/1000/ 2>&1 | sed 's/^/SESSRUN /'
echo SESSDONE
PROBE
python3 -I scripts/qemu-drive.py -m 2G -t 400 --log /tmp/session-test.log \
  --cmdline "aur.session=user:alvaro" --done SESSDONE \
  --boot-test '(bash /usr/bin/session-probe.sh &)' >/tmp/session-test.out 2>&1
rm -f sysroot/usr/bin/session-probe.sh
grep -a "SESSPROC\|SESSRUN\|init: sesion\|aurora-wayland\]" /tmp/session-test.log | cut -c1-200 | head -40
grep -a "SESSPROC.*aurora-wayland.*1000" /tmp/session-test.log >/dev/null
