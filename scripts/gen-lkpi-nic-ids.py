#!/usr/bin/env python3
"""Genera kernel/lkpi_nic_ids.h: tabla PCI ID -> modulo .kur de los drivers de
red de Linux portados (Plan_Red.md), leida de las pci_device_id de sus fuentes.
Uso: scripts/gen-lkpi-nic-ids.py   (necesita build/linuxkpi_src/linux)."""
import re, os, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "build/linuxkpi_src/linux")
# modulo -> (fuente con la tabla, nombre de la tabla, directorios de cabeceras)
DRIVERS = [
    ("lkpi_r8169", "drivers/net/ethernet/realtek/r8169_main.c", "rtl8169_pci_tbl", ["drivers/net/ethernet/realtek"]),
    ("lkpi_e1000e", "drivers/net/ethernet/intel/e1000e/netdev.c", "e1000_pci_tbl", ["drivers/net/ethernet/intel/e1000e"]),
    ("lkpi_igc", "drivers/net/ethernet/intel/igc/igc_main.c", "igc_pci_tbl", ["drivers/net/ethernet/intel/igc"]),
    ("lkpi_e1000", "drivers/net/ethernet/intel/e1000/e1000_main.c", "e1000_pci_tbl", ["drivers/net/ethernet/intel/e1000"]),
    ("lkpi_igb", "drivers/net/ethernet/intel/igb/igb_main.c", "igb_pci_tbl", ["drivers/net/ethernet/intel/igb"]),
    ("lkpi_alx", "drivers/net/ethernet/atheros/alx/main.c", "alx_pci_tbl", ["drivers/net/ethernet/atheros/alx"]),
]
# Atendidos por un .aur nativo: el .aur manda salvo que aur.skip lo omita (entonces, Linux).
NATIVE = {(0x8086, 0x10d3)}
defs = {}
def scan_defs(path):
    try: txt = open(path, errors="replace").read()
    except OSError: return
    for m in re.finditer(r"^\s*#\s*define\s+(\w+)\s+\(?\s*(0x[0-9a-fA-F]+|\d+)\s*\)?\s*(?:/\*.*)?$", txt, re.M):
        defs.setdefault(m.group(1), int(m.group(2), 0))
scan_defs(os.path.join(SRC, "include/linux/pci_ids.h"))
out = []
# Wi-Fi Intel: iwl_hw_card_ids[] con IWL_PCI_DEVICE(dev, subdev, cfg); solo las
# configuraciones MVM compiladas en lkpi_iwlwifi (cfg/7000..sc).
WIFI = "lkpi_cfg80211,lkpi_mac80211,lkpi_iwlwifi,lkpi_iwlmvm"
IWL = os.path.join(SRC, "drivers/net/wireless/intel/iwlwifi")
have = set()
for c in ("7000", "8000", "9000", "22000", "ax210", "bz", "sc"):
    have |= set(re.findall(r"const struct iwl_mac_cfg (\w+)\s*=", open(os.path.join(IWL, "cfg", c + ".c")).read()))
tbl = re.search(r"iwl_hw_card_ids\[\]\s*=\s*\{(.*?)\n\};", open(os.path.join(IWL, "pcie/drv.c")).read(), re.S).group(1)
for m in re.finditer(r"IWL_PCI_DEVICE\(\s*(0x[0-9a-fA-F]+)\s*,\s*\w+\s*,\s*(\w+)\s*\)", tbl):
    if m.group(2) in have: out.append((0x8086, int(m.group(1), 16), WIFI))
# virtio-net (transicional 1af4:1000 y moderno 1af4:1041) sobre el bus virtio de la capa.
for dev in (0x1000, 0x1041):
    out.append((0x1af4, dev, "lkpi_virtio,lkpi_virtio_net"))
for mod, f, tbl, dirs in DRIVERS:
    for d in dirs:
        for fn in os.listdir(os.path.join(SRC, d)):
            if fn.endswith(".h") or fn.endswith(".c"): scan_defs(os.path.join(SRC, d, fn))
    txt = open(os.path.join(SRC, f)).read()
    m = re.search(tbl + r"\[\]\s*=\s*\{(.*?)\n\};", txt, re.S)
    if not m: sys.exit("no encuentro " + tbl)
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    for e in re.finditer(r"INTEL_E1000_ETHERNET_DEVICE\(\s*(0x[0-9a-fA-F]+)\s*\)", body):
        out.append((0x8086, int(e.group(1), 16), mod))
    for e in re.finditer(r"\{([^{}]*)\}", body):
        raw = e.group(1).strip()
        if not raw or raw == "0": continue
        def val(x):
            x = x.strip()
            if re.fullmatch(r"0x[0-9a-fA-F]+|\d+", x): return int(x, 0)
            return defs.get(x)
        vend = dev = None
        mm = re.match(r"INTEL_E1000_ETHERNET_DEVICE\(\s*(\w+)\s*\)", raw)
        if mm:
            out.append((0x8086, val(mm.group(1)), mod)); continue
        mm = re.match(r"PCI_VDEVICE\(\s*(\w+)\s*,\s*(\w+)\s*\)", raw)
        if mm:
            vend = defs.get("PCI_VENDOR_ID_" + mm.group(1)); dev = val(mm.group(2))
        else:
            mm = re.match(r"PCI_DEVICE\(\s*(\w+)\s*,\s*(\w+)\s*\)", raw)
            if mm: vend = val(mm.group(1)); dev = val(mm.group(2))
            else:
                a = [x.strip() for x in raw.split(",")]
                if len(a) >= 2: vend, dev = val(a[0]), val(a[1])
        if vend is None or dev is None:
            print("aviso: entrada sin resolver en %s: %s" % (mod, e.group(1)), file=sys.stderr); continue
        out.append((vend, dev, mod))
seen = set(); rows = []
for v, d, m in out:
    if (v, d) in seen: continue
    seen.add((v, d)); rows.append((v, d, m))
rows.sort()
with open(os.path.join(ROOT, "kernel/lkpi_nic_ids.h"), "w") as fh:
    fh.write("// Generado por scripts/gen-lkpi-nic-ids.py. No editar.\n// PCI ID de las NIC que atienden los drivers de red de Linux (Plan_Red.md).\n")
    fh.write("static const struct { uint16_t vendor, device; uint8_t native; const char *module; } lkpi_nic_ids[] = {\n")
    for v, d, m in rows: fh.write('    {0x%04x, 0x%04x, %d, "%s"},\n' % (v, d, 1 if (v, d) in NATIVE else 0, m))
    fh.write("};\n")
print(len(rows), "IDs")
