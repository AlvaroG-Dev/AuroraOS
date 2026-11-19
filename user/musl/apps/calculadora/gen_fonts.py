#!/usr/bin/env python3
# Genera calc_fonts.h: Inter rasterizada a los tamaños que usa la Calculadora
# (solo los caracteres que aparecen), para no escalar la fuente de 14 px.
#   python3 user/musl/apps/calculadora/gen_fonts.py > user/musl/apps/calculadora/calc_fonts.h
import sys
from PIL import Image, ImageDraw, ImageFont

TTF = 'kernel/gfx/pre-fonts-tmp/Inter-VariableFont_opsz,wght.ttf'
CHARS = ' 0123456789,.-+/x%=ACEeor'
# (nombre, tamaño px, peso)
FONTS = [('calc_disp44', 44, 300), ('calc_disp36', 36, 300),
         ('calc_disp30', 30, 300), ('calc_disp24', 24, 300),
         ('calc_key', 24, 400), ('calc_expr', 16, 400)]

out = ['// GENERADO por gen_fonts.py (Inter). No editar a mano.',
       '#pragma once', '#include "../../../lib/font_aa.h"', '']
for name, size, wght in FONTS:
    f = ImageFont.truetype(TTF, size)
    try:
        f.set_variation_by_axes([min(max(size, 14), 32), wght])
    except Exception as e:
        print('aviso: %s' % e, file=sys.stderr)
    glyphs = {}
    for ch in CHARS:
        c = ord(ch)
        adv = int(round(f.getlength(ch)))
        bb = f.getbbox(ch)
        if ch == ' ' or not bb or bb[2] <= bb[0] or bb[3] <= bb[1]:
            glyphs[c] = (0, 0, 0, 0, adv, None)
            continue
        w, h = bb[2] - bb[0], bb[3] - bb[1]
        img = Image.new('L', (w, h), 0)
        ImageDraw.Draw(img).text((-bb[0], -bb[1]), ch, font=f, fill=255)
        px = list(img.getdata())
        var = '%s_g%d' % (name, c)
        out.append('static const uint8_t %s[] = {' % var)
        for i in range(0, len(px), 16):
            out.append('  ' + ','.join('%d' % p for p in px[i:i + 16]) + ',')
        out.append('};')
        glyphs[c] = (w, h, bb[0], size - bb[1], adv, var)
    out.append('static const font_aa_t %s = {' % name)
    out.append('  .height = %d,' % size)
    out.append('  .glyphs = {')
    for c, (w, h, bx, by, adv, var) in sorted(glyphs.items()):
        out.append('    [%d] = {%d, %d, %d, %d, %d, %s},' % (c, w, h, bx, by, adv, var or '0'))
    out.append('  }')
    out.append('};')
    out.append('')
print('\n'.join(out))
