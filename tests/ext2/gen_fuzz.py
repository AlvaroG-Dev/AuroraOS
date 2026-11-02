#!/usr/bin/env python3
"""Genera imágenes ext2 mutadas (corruptas) para probar que el kernel no se cae.

  gen_fuzz.py <dir_salida> <semilla> <n_imágenes>

Parte de una imagen pequeña (1 KiB, 16 MiB) hecha con mke2fs -d y le aplica,
por imagen, mutaciones al azar: bytes en superbloque/GDT, tabla de inodos,
bloques de directorio e indirectos, más algunas mutaciones dirigidas (rec_len
a 0, punteros fuera de rango, descriptores de grupo absurdos, directorio que
apunta a su ancestro).
"""
import os, random, re, struct, subprocess, sys, shutil, hashlib

out, seed, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
os.makedirs(out, exist_ok=True)
tree = os.path.join(out, "tree")
shutil.rmtree(tree, ignore_errors=True)
for d in ("a/b/c", "empty"):
    os.makedirs(os.path.join(tree, d))
def det(nbytes, s):
    b = bytearray(); i = 0
    while len(b) < nbytes:
        b += hashlib.sha256(f"{s}:{i}".encode()).digest(); i += 1
    return bytes(b[:nbytes])
for name, size in (("small", 100), ("one", 1024), ("direct", 11 * 1024),
                   ("ind", 100 * 1024), ("dind", 400 * 1024)):
    open(os.path.join(tree, name), "wb").write(det(size, name))
open(os.path.join(tree, "a/b/c/leaf"), "w").write("hoja\n")
for i in range(80):
    open(os.path.join(tree, f"a/f{i:03d}"), "w").write(str(i))
os.symlink("small", os.path.join(tree, "fast"))
os.symlink("x" * 100, os.path.join(tree, "slow"))
os.symlink("a/b", os.path.join(tree, "dl"))

base = os.path.join(out, "base.img")
subprocess.run(["truncate", "-s", "16M", base], check=True)
subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "1024", "-N", "512",
                "-d", tree, base], check=True, stderr=subprocess.DEVNULL)

def blocks_of(path):
    r = subprocess.run(["debugfs", "-R", f"stat {path}", base],
                       capture_output=True, text=True).stdout
    m = re.search(r"BLOCKS:\n(.*?)\nTOTAL", r, re.S)
    if not m:
        return []
    return [int(x) for x in re.findall(r"(?:\(\w+\))?:?(\d+)", m.group(1).replace("(", " ("))
            if int(x) < 16384][:40]

meta_blocks = set()
for p in ("<2>", "/a", "/a/b", "/a/b/c", "/ind", "/dind", "/direct", "/slow"):
    meta_blocks.update(blocks_of(p))
meta_blocks = sorted(meta_blocks)
# tabla de inodos del grupo 0 (los primeros bloques tras bitmaps)
dump = subprocess.run(["dumpe2fs", base], capture_output=True, text=True).stdout
m = re.search(r"Inode table at (\d+)-(\d+)", dump)
itab = (int(m.group(1)), int(m.group(2))) if m else (5, 20)

rnd = random.Random(seed)
for k in range(n):
    img = os.path.join(out, f"fuzz{k}.img")
    shutil.copyfile(base, img)
    with open(img, "r+b") as f:
        def poke(off, data):
            f.seek(off); f.write(data)
        def rbyte():
            return bytes([rnd.randrange(256)])
        kind = k % 6
        if kind == 0:    # bytes al azar en superbloque + GDT
            for _ in range(rnd.randrange(1, 12)):
                poke(1024 + rnd.randrange(0, 2048), rbyte())
        elif kind == 1:  # campos clave del superbloque a valores extremos
            for off, fmt in ((1024 + 0, "<I"), (1024 + 4, "<I"), (1024 + 20, "<I"),
                             (1024 + 24, "<I"), (1024 + 32, "<I"), (1024 + 40, "<I"),
                             (1024 + 88, "<H"), (1024 + 76, "<I")):
                if rnd.random() < 0.3:
                    poke(off, struct.pack(fmt, rnd.choice([0, 1, 0xFFFF, 0x7FFFFFFF, 0xFFFFFFFF, rnd.randrange(1 << 20)]) & ((1 << (8 * struct.calcsize(fmt))) - 1)))
        elif kind == 2:  # tabla de inodos
            for _ in range(rnd.randrange(1, 30)):
                poke(itab[0] * 1024 + rnd.randrange(0, 40 * 256), rbyte())
        elif kind == 3:  # bloques de directorio / indirectos / symlink lento
            for _ in range(rnd.randrange(1, 30)):
                b = rnd.choice(meta_blocks)
                poke(b * 1024 + rnd.randrange(0, 1024), rbyte())
        elif kind == 4:  # rec_len = 0 / enorme en bloques de directorio
            for b in meta_blocks[:6]:
                if rnd.random() < 0.7:
                    poke(b * 1024 + 4 + 12 * rnd.randrange(0, 3), struct.pack("<H", rnd.choice([0, 3, 11, 0xFFFF, 1025])))
        else:            # punteros de bloque fuera de rango en inodos
            for ino in range(2, 20):
                off = itab[0] * 1024 + (ino - 1) * 256 + 40 + 4 * rnd.randrange(0, 15)
                if rnd.random() < 0.5:
                    poke(off, struct.pack("<I", rnd.choice([0xFFFFFFFF, 1, 16384, 0x7FFFFFFF, rnd.randrange(1 << 24)])))
    print(img)
