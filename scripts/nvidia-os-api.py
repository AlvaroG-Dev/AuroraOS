#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""nvidia-os-api.py — interfaz de SO que exige el nucleo abierto de NVIDIA.

Plan_NVIDIA.md, T0.2. Analisis lexico de open-gpu-kernel-modules (version
fijada, build/linuxkpi_src/nvidia-open): extrae los prototipos de
os-interface.h (os_*), de nv.h (nv_* implementados por la capa de SO; rm_* los
implementa el RM) y de nvidia-modeset-os-interface.h (nvkms_*), cuenta sus
usos en src/ y dice cual implementa hoy kernel-open/ (la capa Linux).

Uso: scripts/nvidia-os-api.py [--nv DIR] [--md docs/nv_os_api.md]
"""
import argparse, collections, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def read(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return f.read()

def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)

PROTO = re.compile(r'^[ \t]*([A-Za-z_][\w \t\*]*?)[ \t]*\*?[ \t]*NV_API_CALL[ \t]+\*?[ \t]*(\w+)[ \t]*\(([^;{]*?)\)[ \t]*;', re.M | re.S)
PROTO2 = re.compile(r'^[ \t]*([A-Za-z_][\w \t\*]*?)[ \t]+\*?[ \t]*(nvkms_\w+)[ \t]*\(([^;{]*?)\)[ \t]*;', re.M | re.S)

def protos(path, rx):
    out = collections.OrderedDict()
    for m in rx.finditer(strip_comments(read(path))):
        name = m.group(2)
        if name not in out:
            out[name] = (' '.join(m.group(1).split()), ' '.join(m.group(3).split()))
    return out

def walk(base, exts=('.c', '.h', '.cpp')):
    for d, _, fs in os.walk(base):
        for f in fs:
            if f.endswith(exts):
                yield os.path.join(d, f)

def count_uses(names, bases):
    rx = re.compile(r'\b(' + '|'.join(map(re.escape, names)) + r')\b')
    c = {b: collections.Counter() for b in bases}
    for b in bases:
        for p in walk(b):
            for m in rx.finditer(strip_comments(read(p))):
                c[b][m.group(1)] += 1
    return c

def defined_in(names, base):
    """Ficheros de `base` que DEFINEN cada nombre (nombre( ... ) {)."""
    rx = re.compile(r'\b(' + '|'.join(map(re.escape, names)) + r')\s*\([^;{}]*\)\s*\{')
    d = collections.defaultdict(set)
    for p in walk(base):
        for m in rx.finditer(strip_comments(read(p))):
            d[m.group(1)].add(os.path.relpath(p, base))
    return d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nv', default=os.path.join(ROOT, 'build/linuxkpi_src/nvidia-open'))
    ap.add_argument('--md', default=os.path.join(ROOT, 'docs/nv_os_api.md'))
    a = ap.parse_args()
    nv = a.nv
    ver = read(os.path.join(nv, '.aurora-version')).strip() if os.path.exists(os.path.join(nv, '.aurora-version')) else '?'
    src = os.path.join(nv, 'src')
    ko = os.path.join(nv, 'kernel-open')
    osi = protos(os.path.join(src, 'nvidia/arch/nvalloc/unix/include/os-interface.h'), PROTO)
    nvh = protos(os.path.join(src, 'nvidia/arch/nvalloc/unix/include/nv.h'), PROTO)
    kms = protos(os.path.join(src, 'nvidia-modeset/os-interface/include/nvidia-modeset-os-interface.h'), PROTO2)
    # nvkms_* tambien con prototipo sin NV_API_CALL
    kms.update({k: v for k, v in protos(os.path.join(src, 'nvidia-modeset/os-interface/include/nvidia-modeset-os-interface.h'), PROTO).items() if k.startswith('nvkms_')})
    nv_os = collections.OrderedDict((k, v) for k, v in nvh.items() if not k.startswith('rm_'))
    rm = collections.OrderedDict((k, v) for k, v in nvh.items() if k.startswith('rm_'))

    bases = [os.path.join(src, 'nvidia'), os.path.join(src, 'common'), os.path.join(src, 'nvidia-modeset')]
    names = list(osi) + list(nv_os) + list(kms)
    uses = count_uses(names, bases)
    def total(n): return sum(uses[b][n] for b in bases)
    impl = defined_in(names, ko)

    L = []
    w = L.append
    w('# Interfaz de SO del nucleo abierto de NVIDIA (Plan_NVIDIA.md, T0.2)\n')
    w(f'Generado por `scripts/nvidia-os-api.py` sobre NVIDIA open **{ver}** (fuentes en `build/linuxkpi_src/nvidia-open`, no versionadas). Analisis lexico: los usos cuentan menciones en `src/nvidia`, `src/common` y `src/nvidia-modeset` (incluye la propia declaracion y los `#define`).\n')
    w('Columna **Linux** = fichero de `kernel-open/` que lo define hoy (lo que habria que reescribir en una capa propia, o lo que correria tal cual sobre LinuxKPI si se elige `kernel-open/`). Columna **Disposicion** la rellena el autor/Claude en T0.2b (ver abajo).\n')
    def table(title, d, note):
        w(f'\n## {title} ({len(d)})\n')
        w(note + '\n')
        w('| Funcion | Usos | Linux | Prototipo |')
        w('|---|---:|---|---|')
        for n, (ret, args) in d.items():
            u = total(n)
            im = ', '.join(sorted(impl.get(n, []))) or '—'
            sig = f'{ret} {n}({args})'.replace('|', '\\|')
            w(f'| `{n}` | {u} | {im} | `{sig}` |')
    table('os_* (el RM llama a la capa de SO)', osi, 'Declaradas en `src/nvidia/arch/nvalloc/unix/include/os-interface.h`. Una capa propia debe implementarlas **todas** las que tengan usos > 1 (>1 porque la declaracion cuenta).')
    table('nv_* (servicios de la capa de SO que el RM invoca)', nv_os, 'Declaradas en `nv.h` con `NV_API_CALL`; implementadas por la capa Linux (`nv.c`, `nv-vm.c`, `nv-mmap.c`, `nv-dma.c`...).')
    w(f'\n## rm_* (el RM las exporta; la capa de SO las llama) ({len(rm)})\n')
    w('Implementadas por `src/nvidia` (no hay que escribirlas): son los puntos de entrada del RM.\n')
    w('| Funcion | Prototipo |')
    w('|---|---|')
    for n, (ret, args) in rm.items():
        w(f'| `{n}` | `{(ret + " " + n + "(" + args + ")").replace("|", chr(92) + "|")}` |')
    table('nvkms_* (nvidia-modeset llama a su capa de SO)', kms, 'Declaradas en `src/nvidia-modeset/os-interface/include/nvidia-modeset-os-interface.h`; las implementa `kernel-open/nvidia-modeset/nvidia-modeset-linux.c`.')

    # Resumen
    unused = [n for n in osi if total(n) <= 1]
    w('\n## Resumen\n')
    w(f'- `os_*`: {len(osi)} funciones; {len(unused)} sin usos aparte de la declaracion (candidatas a stub): ' + (', '.join(f'`{n}`' for n in unused) or 'ninguna') + '.')
    w(f'- `nv_*` de la capa de SO: {len(nv_os)}; `rm_*` del RM: {len(rm)}; `nvkms_*`: {len(kms)}.')
    tot = collections.Counter()
    for n in osi:
        tot['os'] += 1 if impl.get(n) else 0
    w(f'- `os_*` definidas en `kernel-open/`: {tot["os"]} de {len(osi)}.')
    os.makedirs(os.path.dirname(a.md), exist_ok=True)
    with open(a.md, 'w', encoding='utf-8') as f:
        f.write('\n'.join(L) + '\n')
    print(f'{a.md}: os={len(osi)} nv={len(nv_os)} rm={len(rm)} nvkms={len(kms)}')

if __name__ == '__main__':
    main()
