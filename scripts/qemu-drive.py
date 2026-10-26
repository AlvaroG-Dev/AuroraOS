#!/usr/bin/env python3
"""Arranca aurora.iso en QEMU (sin pantalla) y la maneja por pasos.

Uso: qemu-drive.py [-t seg] [-m MB] --boot-test "orden" ... --step MARCADOR=cmd;cmd ...

Cada --step espera a que MARCADOR aparezca en el log serie y entonces envia las
ordenes (separadas por ';') al monitor de QEMU. Ordenes especiales: `sleep N`
y `shot FICHERO.ppm` (screendump). Termina al ver el marcador DONE o por
timeout. Sin KVM (TCG) todo es lento: usar plazos largos.
"""
import argparse, os, socket, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('-t', type=int, default=600)
ap.add_argument('-m', default='512M')
ap.add_argument('--log', default='/tmp/boot-test.log')
ap.add_argument('--boot-test', action='append', default=[])
ap.add_argument('--step', action='append', default=[])
ap.add_argument('--done', default='DRIVE-DONE')
ap.add_argument('--qemu', action='append', default=[], help='argumento extra para QEMU (repetible)')
ap.add_argument('--extra', action='append', default=[], help='argumento extra para QEMU (repetible), p. ej. --extra=-vga --extra=none')
a = ap.parse_args()
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(root)

bt = os.path.join('sysroot', 'system', 'boot-tests')
if a.boot_test:
    open(bt, 'w').write('\n'.join(a.boot_test) + '\n')
r = subprocess.run(['make', 'iso', 'MUSL_CC=/usr/bin/musl-gcc', 'KUR_CC=gcc'],
                   stdout=open('/tmp/drive-build.log', 'w'), stderr=subprocess.STDOUT)
if os.path.exists(bt):
    os.remove(bt)
if r.returncode:
    sys.exit('build fallo, ver /tmp/drive-build.log')

subprocess.run(['cp', '/usr/share/OVMF/OVMF_VARS_4M.fd', 'OVMF_VARS.fd'])
for f in (a.log, '/tmp/qmon.sock'):
    if os.path.exists(f):
        os.remove(f)
q = subprocess.Popen([
    'qemu-system-x86_64',
    '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
    '-drive', 'if=pflash,format=raw,file=OVMF_VARS.fd',
    '-m', a.m, '-no-reboot', '-smp', '2', '-cdrom', 'aurora.iso',
    '-display', 'none', '-monitor', 'unix:/tmp/qmon.sock,server,nowait',
    '-serial', 'file:' + a.log] + a.extra)

for _ in range(60):
    if os.path.exists('/tmp/qmon.sock'):
        break
    time.sleep(0.5)
s = socket.socket(socket.AF_UNIX)
s.connect('/tmp/qmon.sock')
s.settimeout(0.2)

def mon(cmd):
    s.send((cmd + '\n').encode())
    time.sleep(0.4)
    try:
        s.recv(65536)
    except Exception:
        pass

def log():
    try:
        return open(a.log, 'rb').read().decode('latin1')
    except Exception:
        return ''

steps = [x.split('=', 1) for x in a.step]
t0 = time.time()
i = 0
try:
    while time.time() - t0 < a.t and q.poll() is None:
        text = log()
        if i < len(steps) and steps[i][0] in text:
            for c in steps[i][1].split(';'):
                c = c.strip()
                if c.startswith('sleep '):
                    time.sleep(float(c.split()[1]))
                elif c.startswith('shot '):
                    mon('screendump ' + c.split(None, 1)[1])
                elif c:
                    mon(c)
            print('paso %d (%s) ejecutado a los %ds' % (i + 1, steps[i][0], time.time() - t0), flush=True)
            i += 1
            continue
        if a.done in text:
            break
        time.sleep(1)
finally:
    q.terminate()
    try:
        q.wait(10)
    except Exception:
        q.kill()
print('pasos hechos: %d/%d' % (i, len(steps)))
