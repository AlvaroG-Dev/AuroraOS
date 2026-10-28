#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Lista los .c que compila el Makefile (Kbuild) de un driver de Linux con un
conjunto fijo de CONFIG_* activos.  Uso:
  lkpi-kbuild-srcs.py <dir-driver> <objeto-final> CONFIG_A CONFIG_B ...
Solo entiende `x-y += a.o b.o`, `x-$(CONFIG_X) += ...` y `include $(src)/sub/Makefile`;
las condiciones ifdef/ifeq no se evalúan (se ignoran los bloques que no son listas)."""
import os, re, sys

def parse(path, cfg, out, base=""):
    text = open(path, encoding="utf-8", errors="replace").read().replace("\\\n", " ")
    for line in text.splitlines():
        line = line.split("#")[0].strip()
        m = re.match(r"include\s+\$\(src\)/(\S+)", line)
        if m:
            sub = os.path.join(os.path.dirname(path), m.group(1))
            parse(sub, cfg, out, base)
            continue
        m = re.match(r"([\w\-./]+?)-(y|\$\((\w+)\))\s*\+?:?=\s*(.*)", line)
        if not m:
            continue
        obj, cond, conf, rest = m.groups()
        if cond != "y" and conf not in cfg:
            continue
        for tok in rest.split():
            mm = re.match(r"\$\((\w+)-y\)$", tok)
            if mm:
                out.setdefault(obj, []).append(mm.group(1) + ".o")
                continue
            if tok.endswith(".o") or tok.endswith("/"):
                out.setdefault(obj, []).append(os.path.join(base, tok))

def main():
    d, final = sys.argv[1], sys.argv[2]
    cfg = set(sys.argv[3:])
    out = {}
    parse(os.path.join(d, "Makefile"), cfg, out)
    seen, todo, files = set(), [final], []
    while todo:
        o = todo.pop(0)
        if o in seen:
            continue
        seen.add(o)
        for tok in out.get(o, []):
            name = tok[:-2] if tok.endswith(".o") else tok
            if name in out:       # objeto compuesto (p. ej. gt, gem)
                todo.append(name)
            elif tok.endswith(".o") and os.path.exists(os.path.join(d, name + ".c")):
                files.append(name + ".c")
    for f in dict.fromkeys(files):
        print(f)

main()
