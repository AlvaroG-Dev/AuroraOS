#!/usr/bin/env python3
"""Resuelve y descarga paquetes de Ubuntu 24.04 (noble) SIN depender de apt del host
(el host de Alvaro es Kali: sus paquetes llevan otra glibc). Uso:
  ubuntu-fetch-debs.py DESTDIR SKIP_REGEX pkg [pkg...]
Descarga el cierre transitivo de Depends/Pre-Depends en DESTDIR/*.deb."""
import gzip, os, re, sys, urllib.request
dest, skip = sys.argv[1], re.compile(sys.argv[2])
want = sys.argv[3:]
BASE = os.environ.get("UBUNTU_MIRROR", "http://archive.ubuntu.com/ubuntu")
IDX = []
for suite in ("noble-updates", "noble-security", "noble"):
    for comp in ("main", "universe"):
        IDX.append((suite, comp))
pk = {}; prov = {}
cache = os.path.join(dest, ".index"); os.makedirs(cache, exist_ok=True)
for suite, comp in IDX:
    f = os.path.join(cache, f"{suite}-{comp}.gz")
    if not os.path.exists(f):
        with urllib.request.urlopen(f"{BASE}/dists/{suite}/{comp}/binary-amd64/Packages.gz") as r, open(f, "wb") as o:
            o.write(r.read())
    txt = gzip.open(f, "rt", encoding="utf-8", errors="replace").read()
    for blk in txt.split("\n\n"):
        d = {}
        for ln in blk.split("\n"):
            if ln[:1] in " \t" or ":" not in ln: continue
            k, v = ln.split(":", 1); d[k] = v.strip()
        n = d.get("Package")
        if not n or n in pk: continue   # primera suite = la mas nueva
        pk[n] = d
        for p in d.get("Provides", "").split(","):
            p = p.split("(")[0].strip()
            if p: prov.setdefault(p, n)
def deps(d):
    out = []
    for fld in ("Depends", "Pre-Depends"):
        for alt in d.get(fld, "").split(","):
            c = alt.split("|")[0].strip()          # primera alternativa
            c = re.sub(r"\(.*?\)|:any|:amd64", "", c).strip()
            if c: out.append(c)
    return out
seen, todo = set(), list(want)
while todo:
    n = todo.pop()
    if n in seen: continue
    if n not in pk: n = prov.get(n, n)
    if n in seen or n not in pk or skip.match(n): continue
    seen.add(n)
    if not os.environ.get('NODEPS'): todo += deps(pk[n])
print(f"[ubuntu-debs] {len(seen)} paquetes")
for w in want:
    if w not in pk and w not in prov: print(f"  (sin paquete: {w})")
for n in sorted(seen):
    fn = pk[n]["Filename"]; out = os.path.join(dest, os.path.basename(fn))
    if os.path.exists(out): continue
    urllib.request.urlretrieve(f"{BASE}/{fn}", out)
