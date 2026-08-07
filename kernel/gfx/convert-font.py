import sys
import os
import re
from PIL import Image, ImageFont, ImageDraw

if len(sys.argv) < 3:
    print("Uso: python generate_font.py <archivo_ttf> <tamaño_pt> [regular|bold]")
    sys.exit(1)

ttf_path = sys.argv[1]
font_size = int(sys.argv[2])
style_arg = sys.argv[3].lower() if len(sys.argv) > 3 else "regular"

is_bold = style_arg in ["bold", "700", "b"]
weight_num = 700 if is_bold else 400
style_name = "bold" if is_bold else "regular"

# Sanitizar el nombre para generar identificadores C únicos
base_filename = os.path.splitext(os.path.basename(ttf_path))[0]
clean_name = re.sub(r'[^a-zA-Z0-9]', '_', base_filename.lower())
clean_name = clean_name.split('_variablefont')[0].split('_static')[0]

struct_var_name = f"font_{clean_name}_{style_name}_{font_size}"
bmp_prefix = f"glyph_bmp_{clean_name}_{style_name}_{font_size}"

# Cargar fuente
font = ImageFont.truetype(ttf_path, font_size)

# Intentar aplicar el peso en fuentes variables
try:
    font.set_variation_by_axes([weight_num])
except Exception:
    pass

print('#pragma once\n#include "../font_aa.h"\n')

glyphs_data = []

for c in range(128):
    char = chr(c)
    bmp_var = f"{bmp_prefix}_{c}"

    if not char.isprintable() and char != ' ':
        print(f"static const uint8_t {bmp_var}[] = {{ 0 }};")
        glyphs_data.append((0, 0, 0, 0, font_size // 2, bmp_var))
        continue

    bbox = font.getbbox(char)
    advance = font.getlength(char)

    if not bbox or bbox[2] - bbox[0] == 0 or bbox[3] - bbox[1] == 0:
        print(f"static const uint8_t {bmp_var}[] = {{ 0 }};")
        glyphs_data.append((0, 0, 0, 0, int(advance), bmp_var))
        continue

    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]

    img = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-bbox[0], -bbox[1]), char, font=font, fill=255)

    pixels = list(img.getdata())
    
    print(f"static const uint8_t {bmp_var}[] = {{")
    for i in range(0, len(pixels), 12):
        chunk = pixels[i:i+12]
        print("    " + ", ".join(f"0x{p:02X}" for p in chunk) + ",")
    print("};")

    bearing_x = bbox[0]
    bearing_y = font_size - bbox[1]
    glyphs_data.append((w, h, bearing_x, bearing_y, int(advance), bmp_var))

print(f"\nstatic const font_aa_t {struct_var_name} = {{")
print(f"    .height = {font_size},")
print("    .glyphs = {")
for g in glyphs_data:
    print(f"        {{ .width = {g[0]}, .height = {g[1]}, .bearing_x = {g[2]}, .bearing_y = {g[3]}, .advance = {g[4]}, .bitmap = {g[5]} }},")
print("    }\n};")