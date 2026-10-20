# generate_logo.py
from PIL import Image, ImageDraw, ImageFont
import os

FONT_PATH = "kernel/gfx/pre-fonts-tmp/JetBrainsMono-Bold.ttf"
FONT_SIZE = 64
TEXT = "AuroraOS"

if not os.path.exists(FONT_PATH):
    print(f"Error: No se encuentra la fuente en {FONT_PATH}")
    exit(1)

font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
bbox = font.getbbox(TEXT)
w = bbox[2] - bbox[0]
h = bbox[3] - bbox[1]

img = Image.new("RGBA", (w + 12, h + 12), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)
draw.text((-bbox[0] + 6, -bbox[1] + 6), TEXT, font=font, fill=(255, 255, 255, 255))

with open("logo_auroraos.h", "w") as f:
    f.write("/* logo_auroraos.h - Auto-generado (Alpha Mask 8-bit) */\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"#define LOGO_W {img.width}\n")
    f.write(f"#define LOGO_H {img.height}\n\n")
    f.write("static const uint8_t logo_data[] = {\n")
    
    pixels = img.load()
    for y in range(img.height):
        f.write("    ")
        for x in range(img.width):
            r, g, b, a = pixels[x, y]
            # Solo guardamos el canal Alpha (0-255)
            f.write(f"0x{a:02X}, ")
        f.write("\n")
    f.write("};\n")
print(f"Generado logo_auroraos.h ({img.width}x{img.height}) - {img.width*img.height} bytes.")