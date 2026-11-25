# Estres tipo aurora-vm: fichero disperso grande y escrituras de 4-64 KiB en
# desorden (como el IDE de una VM instalando Windows), luego se relee todo.
import os, random, struct, sys
path = sys.argv[1]; total_mb = int(sys.argv[2]); seed = int(sys.argv[3])
random.seed(seed)
SIZE = 40 << 30
fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
os.ftruncate(fd, SIZE)
ver = {}
klist = []
def blk(b, v):
    return struct.pack('<QQ', b, v) * 256
written = 0; n = 0
regions = [random.randrange(0, SIZE // 4096 - 100000) for _ in range(6)]
while written < total_mb << 20:
    nb = random.choice([1, 2, 8, 16, 16, 16])
    r = random.random()
    if r < 0.5:
        b0 = random.choice(regions) + random.randrange(0, 60000)
    else:
        b0 = random.randrange(0, 3 << 18)  # primeros 3 GiB
    data = bytearray()
    for i in range(nb):
        v = ver.get(b0 + i, 0) + 1; ver[b0 + i] = v
        if v == 1: klist.append(b0 + i)
        data += blk(b0 + i, v)
    os.pwrite(fd, bytes(data), b0 * 4096)
    written += len(data); n += 1
    if random.random() < 0.3:  # lecturas de 4-128 KiB como las del IDE
        rb = random.choice(klist) if random.random() < 0.7 else b0
        rn = random.choice([1, 2, 8, 16, 32])
        got = os.pread(fd, rn * 4096, rb * 4096)
        for i in range(rn):
            v = ver.get(rb + i)
            if v and got[i*4096:(i+1)*4096] != blk(rb + i, v):
                gb, gv = struct.unpack('<QQ', got[i*4096:i*4096+16])
                print('@@MAL en marcha', n, rb + i, v, gb, gv, flush=True)
    if n % 5000 == 0:
        print('@@PROGRESO', n, written >> 20, flush=True)
bad = 0; first = []
keys = sorted(ver)
for b in keys:
    got = os.pread(fd, 4096, b * 4096)
    if got != blk(b, ver[b]):
        bad += 1
        if len(first) < 8:
            gb, gv = struct.unpack('<QQ', got[:16])
            first.append((b, ver[b], gb, gv))
print('@@ESTRES escrituras=%d bloques=%d mal=%d' % (n, len(ver), bad), first)
os.close(fd)
