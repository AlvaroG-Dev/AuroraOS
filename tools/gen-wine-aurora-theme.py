#!/usr/bin/env python3
"""Genera el estilo visual de Aurora para Wine (aurora-{dark,light}.msstyles).

Parte del aero.msstyles que trae Wine (plano, blanco y azul) y lo pasa a la
paleta de Aurora: superficies grafito (oscuro) o blanco verdoso (claro), bordes
suaves y el verde aurora #3EE89B donde Wine usa azul. Cambia los bitmaps, los
colores del .ini del tema y los colores del sistema (los mismos que
installer/wine/aurora-{dark,light}.reg).

Uso: gen-wine-aurora-theme.py <aero.msstyles> <dir-salida>
Necesita llvm-rc y lld-link. El resultado se guarda comprimido en
installer/wine/aurora-*.msstyles.xz (el Makefile solo lo descomprime).
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REGDIR = os.path.join(HERE, "..", "installer", "wine")

GREEN = (62, 232, 155)  # verde aurora
ON_GREEN = (4, 38, 26)  # texto sobre verde (HilightText)
WINE_BLUE = (48, 150, 250)

# Gris de aero (0..255) -> color de Aurora, por tramos. Oscuro: se invierte
# (controles blancos -> grafito, texto negro -> blanco). Claro: se conserva con
# el leve tinte verde de la paleta clara.
RAMPS = {
    "dark": [(0, (240, 240, 240)), (106, (150, 150, 155)), (166, (96, 96, 101)),
             (189, (82, 82, 86)), (227, (62, 62, 66)), (240, (52, 52, 55)),
             (255, (44, 44, 46))],
    "light": [(0, (20, 33, 27)), (106, (100, 110, 105)), (166, (160, 166, 163)),
              (189, (193, 200, 197)), (227, (225, 232, 229)), (245, (244, 247, 245)),
              (255, (255, 255, 255))],
}

# Colores del sistema del .ini de aero -> nombre en el .reg de Aurora.
SYS = {
    "Scrollbar": "Scrollbar", "Background": "Background", "ActiveCaption": "ActiveTitle",
    "InactiveCaption": "InactiveTitle", "Menu": "Menu", "Window": "Window",
    "WindowFrame": "WindowFrame", "MenuText": "MenuText", "WindowText": "WindowText",
    "CaptionText": "TitleText", "ActiveBorder": "ActiveBorder",
    "InactiveBorder": "InactiveBorder", "AppWorkSpace": "AppWorkSpace",
    "Highlight": "Hilight", "HighlightText": "HilightText", "BtnFace": "ButtonFace",
    "BtnShadow": "ButtonShadow", "GrayText": "GrayText", "BtnText": "ButtonText",
    "InactiveCaptionText": "InactiveTitleText", "BtnHighlight": "ButtonHilight",
    "DkShadow3d": "ButtonDkShadow", "Light3d": "ButtonLight", "InfoText": "InfoText",
    "InfoBk": "InfoWindow", "ButtonAlternateFace": "ButtonAlternateFace",
    "HotTracking": "HotTrackingColor", "GradientActiveCaption": "GradientActiveTitle",
    "GradientInactiveCaption": "GradientInactiveTitle", "MenuHilight": "MenuHilight",
    "MenuBar": "MenuBar",
}


def ramp(mode, v):
    pts = RAMPS[mode]
    for (x0, c0), (x1, c1) in zip(pts, pts[1:]):
        if v <= x1:
            t = (v - x0) / (x1 - x0) if x1 > x0 else 0
            return tuple(round(a + (b - a) * t) for a, b in zip(c0, c1))
    return pts[-1][1]


def recolor(mode, r, g, b):
    # Azul de Wine (y sus tintes claros) -> verde, mezclado con la superficie.
    t = 0.0
    if b > r + 8 and b >= g:
        t = min(1.0, (b - r) / (WINE_BLUE[2] - WINE_BLUE[0]))
    base = ramp(mode, max(r, g, b) if t > 0 else round(0.299 * r + 0.587 * g + 0.114 * b))
    if t <= 0:
        return base
    return tuple(round(x + (y - x) * t) for x, y in zip(base, GREEN))


# ---------------------------------------------------------------- PE: lectura
def pe_resources(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    o = pe + 24
    dd = o + (112 if struct.unpack_from("<H", d, o)[0] == 0x20B else 96)
    rva = struct.unpack_from("<I", d, dd + 16)[0]
    secs = [struct.unpack_from("<8sIIII", d, o + opt + 40 * i) for i in range(nsec)]

    def r2o(r):
        for _, vs, va, rs, rp in secs:
            if va <= r < va + max(vs, rs):
                return r - va + rp
        raise ValueError(hex(r))

    base = r2o(rva)

    def name(n):
        if n & 0x80000000:
            p = base + (n & 0x7FFFFFFF)
            ln = struct.unpack_from("<H", d, p)[0]
            return d[p + 2:p + 2 + 2 * ln].decode("utf-16le")
        return n

    out = []

    def walk(off, path):
        nn, ni = struct.unpack_from("<HH", d, base + off + 12)
        for i in range(nn + ni):
            n, e = struct.unpack_from("<II", d, base + off + 16 + 8 * i)
            if e & 0x80000000:
                walk(e & 0x7FFFFFFF, path + [name(n)])
            else:
                drva, dsz = struct.unpack_from("<II", d, base + e)
                out.append((path + [name(n)], d[r2o(drva):r2o(drva) + dsz]))

    walk(0, [])
    return out


# ------------------------------------------------------------- transformación
def recolor_dib(mode, dib):
    hs, w, h, _, bc, comp = struct.unpack_from("<IiiHHI", dib, 0)
    if bc not in (24, 32):
        return dib
    off = hs + (12 if comp == 3 and hs == 40 else 0)
    stride = ((w * bc + 31) // 32) * 4
    px = bytearray(dib)
    step = bc // 8
    rows = [off + y * stride for y in range(abs(h))]
    # Con alfa, los bitmaps pueden venir premultiplicados (rgb <= a): entonces
    # se despremultiplica, se cambia el color y se vuelve a multiplicar.
    premul = bc == 32 and all(
        max(px[r + x * 4:r + x * 4 + 3]) <= px[r + x * 4 + 3]
        for r in rows for x in range(w))
    cache = {}
    for row in rows:
        for x in range(w):
            i = row + x * step
            a = px[i + 3] if bc == 32 else 255
            if bc == 32 and a == 0:
                continue
            key = bytes(px[i:i + 3]) + bytes((a,))
            c = cache.get(key)
            if c is None:
                bb, gg, rr = key[:3]
                if premul:
                    bb, gg, rr = (min(255, v * 255 // a) for v in (bb, gg, rr))
                n = recolor(mode, rr, gg, bb)
                if premul:
                    n = tuple(v * a // 255 for v in n)
                c = cache[key] = bytes((n[2], n[1], n[0]))
            px[i:i + 3] = c
    return bytes(px)


def read_reg(mode):
    vals = {}
    for line in open(os.path.join(REGDIR, f"aurora-{mode}.reg"), encoding="utf-8"):
        m = re.match(r'"(\w+)"="(\d+ \d+ \d+)"', line.strip())
        if m:
            vals[m.group(1)] = m.group(2)
    return vals


def recolor_ini(mode, text):
    reg = read_reg(mode)
    out = []
    section = ""
    for line in text.split("\r\n"):
        s = line.strip()
        if s.startswith("["):
            section = s
        m = re.match(r"^(\s*)(\w+)(\s*=\s*)(\d+) (\d+) (\d+)\s*$", line)
        if m:
            key = m.group(2)
            r, g, b = (int(m.group(i)) for i in (4, 5, 6))
            if section == "[SysMetrics]" and key in SYS and SYS[key] in reg:
                val = reg[SYS[key]]
            elif key == "TextColor" and (r, g, b) == (255, 255, 255):
                val = "%d %d %d" % ON_GREEN  # texto sobre el verde (pulsado)
            else:
                val = "%d %d %d" % recolor(mode, r, g, b)
            line = m.group(1) + key + m.group(3) + val
        elif section == "[documentation]" and s.startswith("DisplayName"):
            line = "DisplayName = Aurora"
        elif section == "[documentation]" and s.startswith("ToolTip"):
            line = "ToolTip = Estilo visual de Aurora"
        out.append(line)
    return "\r\n".join(out)


# ---------------------------------------------------------------- PE: escritura
def rc_id(v):
    return str(v) if isinstance(v, int) else v


def build(res, mode, outpath):
    with tempfile.TemporaryDirectory() as t:
        lines = []
        for i, (path, data) in enumerate(res):
            typ, nam, lang = path
            if typ == 2:
                data = recolor_dib(mode, data)
            elif typ == "TEXTFILE":
                txt = data.decode("utf-16le")
                txt = recolor_ini(mode, txt)
                data = txt.encode("utf-16le")
            elif typ == "COLORNAMES":
                data = "Aurora\0\0".encode("utf-16le")
            f = os.path.join(t, f"r{i}.bin")
            open(f, "wb").write(data)
            lines.append(f"LANGUAGE {lang & 0x3FF}, {lang >> 10}")
            lines.append(f'{rc_id(nam)} {rc_id(typ)} "{f}"')
        rc = os.path.join(t, "theme.rc")
        open(rc, "w").write("\n".join(lines) + "\n")
        subprocess.run(["llvm-rc", "/fo", os.path.join(t, "theme.res"), rc], check=True)
        subprocess.run(["lld-link", "/dll", "/noentry", "/machine:x64", "/nodefaultlib",
                        f"/out:{outpath}", os.path.join(t, "theme.res")], check=True,
                       stdout=subprocess.DEVNULL)


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    res = pe_resources(src)
    os.makedirs(outdir, exist_ok=True)
    for mode in ("dark", "light"):
        out = os.path.join(outdir, f"aurora-{mode}.msstyles")
        build(res, mode, out)
        print(out, os.path.getsize(out))


if __name__ == "__main__":
    main()
