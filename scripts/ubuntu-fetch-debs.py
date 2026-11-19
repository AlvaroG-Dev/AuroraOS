#!/usr/bin/env python3
"""Resuelve y descarga paquetes de Ubuntu 24.04 (noble) SIN depender de apt del host
(el host de Alvaro es Kali: sus paquetes llevan otra glibc). Uso:
  ubuntu-fetch-debs.py DESTDIR SKIP_REGEX pkg [pkg...]
Descarga el cierre transitivo de Depends/Pre-Depends en DESTDIR/*.deb."""
import gzip, os, re, sys, time, urllib.error, urllib.request
dest, skip = sys.argv[1], re.compile(sys.argv[2])
want = sys.argv[3:]
BASE = os.environ.get("UBUNTU_MIRROR", "http://archive.ubuntu.com/ubuntu")
IDX = []
for suite in ("noble-updates", "noble-security", "noble"):
    for comp in ("main", "universe"):
        IDX.append((suite, comp))
cache = os.path.join(dest, ".index"); os.makedirs(cache, exist_ok=True)
MAX_AGE = 6 * 3600   # noble-updates/security cambian: indices viejos dan 404

# archive.ubuntu.com responde 403 al User-Agent por defecto de urllib
# ("Python-urllib/3.x"): se manda uno propio.
UA = {"User-Agent": "aurora-build/1.0 (ubuntu-fetch-debs)"}

def fetch(url, out, tries=4):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r, open(out + ".part", "wb") as o:
                o.write(r.read())
            os.replace(out + ".part", out)
            return
        except urllib.error.HTTPError as e:
            if e.code == 404 or i == tries - 1:
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            if i == tries - 1:
                raise
        time.sleep(2 * (i + 1))

def load_index(force=False):
    pk, prov = {}, {}
    for suite, comp in IDX:
        f = os.path.join(cache, f"{suite}-{comp}.gz")
        if force or not os.path.exists(f) or time.time() - os.path.getmtime(f) > MAX_AGE:
            fetch(f"{BASE}/dists/{suite}/{comp}/binary-amd64/Packages.gz", f)
        parse_index(f, pk, prov)
    return pk, prov

def parse_index(f, pk, prov):
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

pk, prov = load_index()

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
refreshed = False
for n in sorted(seen):
    fn = pk[n]["Filename"]; out = os.path.join(dest, os.path.basename(fn))
    if os.path.exists(out): continue
    try:
        fetch(f"{BASE}/{fn}", out)
    except urllib.error.HTTPError as e:
        if e.code != 404 or refreshed:
            sys.exit(f"[ubuntu-debs] no se pudo bajar {n} ({BASE}/{fn}): {e}")
        # El espejo ya publico otra version: indices nuevos y reintento.
        print(f"[ubuntu-debs] {n}: {os.path.basename(fn)} ya no esta en el espejo, releyendo indices")
        pk, prov = load_index(force=True); refreshed = True
        if n not in pk:
            sys.exit(f"[ubuntu-debs] {n} ya no existe en Ubuntu noble")
        fn = pk[n]["Filename"]; out = os.path.join(dest, os.path.basename(fn))
        try:
            fetch(f"{BASE}/{fn}", out)
        except urllib.error.HTTPError as e2:
            sys.exit(f"[ubuntu-debs] no se pudo bajar {n} ({BASE}/{fn}): {e2}")
