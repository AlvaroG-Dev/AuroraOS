#!/bin/bash
# [U4] El terminal arranca con aur.login=auto:alvaro: su shell debe correr como uid 1000.
# Uso: scripts/login-test.sh [auto:alvaro|prompt|root]    (compila con WITH_NVIDIA=0)
set -u
cd "$(dirname "$0")/.."
export WITH_NVIDIA=0
cat > sysroot/usr/bin/login-probe.sh <<'PROBE'
sleep 25
for d in /proc/[0-9]*; do
  echo "LOGINPROC $(grep -h -E '^(Name|Uid):' $d/status 2>/dev/null | tr '\n\t' '  ')"
done
echo LOGINDONE
PROBE
python3 -I scripts/qemu-drive.py -m 2G -t 400 --log /tmp/login-test.log \
  --cmdline "aur.login=${1:-auto:alvaro}" --done LOGINDONE \
  --boot-test '(bash /usr/bin/login-probe.sh &)' >/tmp/login-test.out 2>&1
rm -f sysroot/usr/bin/login-probe.sh
grep -a "LOGINPROC" /tmp/login-test.log | cut -c1-160
grep -aq "LOGINDONE" /tmp/login-test.log
