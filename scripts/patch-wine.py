#!/usr/bin/env python3
"""Ajustes binarios de Wine para AuroraOS (idempotente).

win32u.so: al crear el escritorio, la primera app arranca explorer.exe y
espera solo 10 s a que este listo (NtUserWaitForInputIdle(proceso, 10000)).
Si explorer tarda mas (carga de Mesa/llvmpipe y Vulkan al enumerar GPUs, un
equipo lento, QEMU), la app falla con "The explorer process failed to start"
y no sale ninguna ventana. Se sube la espera a 120 s: si explorer esta listo
antes, la espera termina en cuanto lo esta.

Uso: patch-wine.py <raiz>   (p. ej. sysroot o build/wine/root)
"""
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else 'sysroot'
path = os.path.join(root, 'usr/lib/wine/x86_64-unix/win32u.so')
if not os.path.exists(path):
    print(f'[patch-wine] {path} no existe; nada que hacer')
    sys.exit(0)

# mov $10000,%esi ; addr32 call NtUserWaitForInputIdle
old = bytes.fromhex('be10270000') + b'\x67\xe8'
new = bytes.fromhex('bec0d40100') + b'\x67\xe8'   # 120000 ms
data = bytearray(open(path, 'rb').read())
n_old, n_new = data.count(old), data.count(new)
if n_old == 0 and n_new == 1:
    print('[patch-wine] win32u.so ya parcheado (espera a explorer 120 s)')
    sys.exit(0)
if n_old != 1:
    print(f'[patch-wine] AVISO: win32u.so no tiene el patron esperado ({n_old} coincidencias); sin parchear')
    sys.exit(0)
i = data.find(old)
data[i:i + len(new)] = new
tmp = path + '.tmp'
with open(tmp, 'wb') as f:
    f.write(data)
os.chmod(tmp, os.stat(path).st_mode)
os.replace(tmp, path)
print(f'[patch-wine] win32u.so: espera a explorer 10 s -> 120 s (offset {i:#x})')
