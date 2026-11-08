#!/bin/bash
# Genera en $1 (por defecto build/ext2-test) un árbol semilla determinista y
# una matriz de imágenes ext2 con mke2fs del host, más el manifiesto esperado
# (stat + sha256) para comparar con lo que ve AuroraOS.
#   tests/ext2/gen_images.sh [directorio_salida]
set -e
OUT=${1:-build/ext2-test}
rm -rf "$OUT"; mkdir -p "$OUT/tree"
T="$OUT/tree"

python3 - "$T" <<'PY'
import os, sys, hashlib
t = sys.argv[1]
def det(n, seed):  # contenido determinista
    out = bytearray(); i = 0
    while len(out) < n:
        out += hashlib.sha256(f"{seed}:{i}".encode()).digest(); i += 1
    return bytes(out[:n])
os.makedirs(f"{t}/sizes")
for n in (0, 1, 511, 1023, 1024, 1025, 4095, 4096, 4097, 12*1024, 12*1024+1,
          268*1024, 268*1024+1, 300*1024, 70000):
    open(f"{t}/sizes/f{n}", "wb").write(det(n, n))
# Fichero grande y disperso: datos en el bloque 0, justo tras el primer
# indirecto, en el doble indirecto y en el triple (con bloque de 1 KiB).
with open(f"{t}/sparse.bin", "wb") as f:
    for off in (0, 12*1024 + 3, 12*1024 + 256*1024 + 5, 70*1024*1024 + 17):
        f.seek(off); f.write(det(5000, off))
    f.truncate(80*1024*1024)
# Directorio con muchas entradas
os.makedirs(f"{t}/many")
for i in range(3000):
    open(f"{t}/many/entry_{i:05d}", "w").write(f"{i}\n")
# Nombres largos
os.makedirs(f"{t}/names")
for n in (1, 2, 100, 127, 128, 200, 255):
    open(f"{t}/names/{'n'*n}", "w").write(f"largo {n}\n")
open(f"{t}/names/acentuado-ñandú-€", "w").write("utf8\n")
# Árbol profundo
d = f"{t}/deep"
for i in range(30): d += f"/d{i}"
os.makedirs(d); open(f"{d}/hoja", "w").write("profundo\n")
# Symlinks: rápidos (<60) y lentos (>=60), absoluto/relativo/intermedio
os.makedirs(f"{t}/links")
for n in (1, 58, 59, 60, 61, 100, 300):
    os.symlink("x"*n, f"{t}/links/sym{n}")
os.symlink("../sizes/f1025", f"{t}/links/rel")
os.symlink("../deep", f"{t}/links/dirlink")
os.symlink("nada", f"{t}/links/colgante")
os.link(f"{t}/sizes/f4097", f"{t}/links/hard4097")
# Modos y propietarios
os.makedirs(f"{t}/perms")
for name, mode in (("m600", 0o600), ("m644", 0o644), ("m755", 0o755),
                   ("m4755", 0o4755), ("m2755", 0o2755), ("m1777", 0o1777)):
    p = f"{t}/perms/{name}"; open(p, "w").write(name + "\n"); os.chmod(p, mode)
os.mkfifo(f"{t}/perms/fifo")
os.chown(f"{t}/perms/m600", 1000, 100)
os.chown(f"{t}/perms/m755", 70000, 70001)   # uid/gid > 65535
os.makedirs(f"{t}/emptydir")
PY

# Manifiesto: ruta tipo(find %y) permisos(octal) uid gid tamaño nlink ; luego sha256.
# (find -printf en vez de stat: en AuroraOS xargs limita los argumentos.)
( cd "$T" && find . -mindepth 1 -printf '%p %y %m %U %G %s %n\n' | LC_ALL=C sort ) > "$OUT/manifest.stat"
( cd "$T" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do
    sha256sum "$f"; done ) > "$OUT/manifest.sha"

mk() { # nombre, tamaño-MiB, opciones...
  local name=$1 mb=$2; shift 2
  rm -f "$OUT/$name.img"; truncate -s ${mb}M "$OUT/$name.img"
  mke2fs -q -F -d "$T" "$@" "$OUT/$name.img"
  e2fsck -fn "$OUT/$name.img" >/dev/null || { echo "e2fsck host falla en $name"; exit 1; }
}
mk r0_1k   160 -t ext2 -r 0 -b 1024
mk r1_1k   160 -t ext2 -b 1024 -O ^dir_index,^resize_inode,^ext_attr,^large_file,^sparse_super
mk r1_1kdef 160 -t ext2 -b 1024
mk r1_2k   160 -t ext2 -b 2048
mk r1_4k   160 -t ext2 -b 4096
mk r1_4k128 160 -t ext2 -b 4096 -I 128
mk r1_g2048 160 -t ext2 -b 1024 -g 2048
mk ext3    160 -t ext3 -b 1024
mk ext4    160 -t ext4 -b 4096
echo "OK: imágenes en $OUT"; ls -la "$OUT"/*.img
