#!/usr/bin/env python3
"""Tema GTK de Aurora: recolorea a verde aurora (sin degradados) y genera la
variante clara. Se puede ejecutar varias veces (es idempotente).

Entrada/salida: precompiled/usr/share/themes/Aurora{,-light}/ y
precompiled/usr/share/aurora/xdg/gtk-4.0/gtk-light.css
Paleta: kernel/gfx/theme.c (k_dark / k_light) y theme.h (acento)."""
import os, re, shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TH = os.path.join(ROOT, "precompiled/usr/share/themes")
XDG = os.path.join(ROOT, "precompiled/usr/share/aurora/xdg")

ACCENT_MAP = {  # azul macOS -> verde aurora
    "#0A84FF": "#3EE89B", "#409CFF": "#6BF083", "#0A6FDB": "#2DB878",
    "#5AAAFF": "#6BF083", "#1A8FFF": "#3EE89B", "#0A5CB8": "#2DB878",
    "#0A4F99": "#1C6B4A",
}
ON_ACCENT = "#04261A"
ACCENT_CTX = re.compile(r"suggested-action|button\.default|:checked|row:selected|"
                        r"\.view:selected|iconview:selected|menuitem:hover|"
                        r"modelbutton:hover|modelbutton:focus|calendar:selected|"
                        r"treeview\.view:selected")


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def write(p, s):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(s)


def flatten_gradients(s):
    return re.sub(r"linear-gradient\(to bottom, (#[0-9A-Fa-f]{6}), (#[0-9A-Fa-f]{6})\)",
                  r"linear-gradient(to bottom, \2, \2)", s)


def recolor_rules(s, fn):
    """Aplica fn(selector, cuerpo) a cada regla `selector { cuerpo }`."""
    return re.sub(r"([^{}]+)\{([^{}]*)\}",
                  lambda m: m.group(1) + "{" + fn(m.group(1), m.group(2)) + "}", s)


BACKDROP = """
/* (selectores repetidos: mas especificidad que los de Adwaita) */
window *:selected:selected:backdrop:backdrop {
  background-color: #2DB878;
  color: #04261A;
}
/* ---- Ventana sin foco: la seleccion conserva el verde (Adwaita usa su azul) ---- */
row:selected:backdrop, treeview.view:selected:backdrop, iconview:selected:backdrop,
list row:selected:backdrop, .view:selected:backdrop, check:checked:backdrop,
radio:checked:backdrop, switch:checked:backdrop, calendar:selected:backdrop {
  background-color: #2DB878;
  color: #04261A;
}
button.suggested-action:backdrop, button.default:backdrop {
  background-image: linear-gradient(to bottom, #2DB878, #2DB878);
  background-color: #2DB878;
  color: #04261A;
}
"""


def dark(s):
    if "mas especificidad que los de Adwaita" not in s and "@define-color aurora_accent " in s:
        s = s.rstrip("\n") + "\n" + BACKDROP
    for a, b in ACCENT_MAP.items():
        s = s.replace(a, b).replace(a.lower(), b)
    s = flatten_gradients(s)

    def on_accent(sel, body):
        if ACCENT_CTX.search(sel) and not sel.strip().startswith("@"):
            body = body.replace("color: #FFFFFF", "color: " + ON_ACCENT)
        return body
    s = recolor_rules(s, on_accent)
    s = s.replace("@define-color theme_selected_fg_color #FFFFFF;",
                  "@define-color theme_selected_fg_color " + ON_ACCENT + ";")
    s = s.replace("@define-color theme_unfocused_selected_fg_color #FFFFFF;",
                  "@define-color theme_unfocused_selected_fg_color " + ON_ACCENT + ";")
    s = s.replace("@define-color accent_fg_color #FFFFFF;",
                  "@define-color accent_fg_color " + ON_ACCENT + ";")
    return s


GTK3_LIGHT_DEFS = {
    "aurora_win": "#F4F7F5", "aurora_titlebar": "#E6EBE9", "aurora_card": "#FFFFFF",
    "aurora_field": "#FFFFFF", "aurora_raised": "#E9EEEC", "aurora_raised_hover": "#DDE4E1",
    "aurora_line": "rgba(0, 0, 0, 0.14)", "aurora_text": "#14211B",
    "aurora_text_2": "rgba(20, 33, 27, 0.80)", "aurora_text_3": "rgba(20, 33, 27, 0.60)",
    "aurora_accent_hover": "#1C8A5C",
    "theme_tooltip_bg_color": "#2E2E2E", "theme_tooltip_fg_color": "#FFFFFF",
    "theme_unfocused_selected_bg_color": "#2DB878",
    "insensitive_bg_color": "#E9EEEC", "insensitive_fg_color": "rgba(20, 33, 27, 0.35)",
    "insensitive_base_color": "#F0F3F1", "insensitive_base_fg_color": "rgba(20, 33, 27, 0.35)",
    "borders": "rgba(0, 0, 0, 0.14)", "unfocused_borders": "rgba(0, 0, 0, 0.10)",
}
LIGHT_LITERALS = {  # grises oscuros de los widgets -> claros
    "#3A3A3A": "#C9D3CE", "#242424": "#E9EEEC", "#2A2A2C": "#FFFFFF",
    "#48484A": "#FFFFFF", "#3A3A3C": "#FFFFFF", "#545456": "#EEF2F0",
    "#444446": "#EEF2F0", "#303032": "#E1E8E4",
}


def wa(m):  # rgba(255,255,255,a) -> negro translucido
    a = float(m.group(1))
    return "rgba(0, 0, 0, %.2f)" % (a * 0.55)


def light_body(sel, body):
    if sel.strip().startswith("tooltip"):
        return body  # el tooltip sigue oscuro
    body = re.sub(r"rgba\(255, 255, 255, ([0-9.]+)\)", wa, body)
    body = re.sub(r"rgba\(0, 0, 0, 0\.(35|40|45)\)", "rgba(0, 0, 0, 0.16)", body)
    for a, b in LIGHT_LITERALS.items():
        body = body.replace(a, b)
    if "selection" in sel and "color: #FFFFFF" in body:
        body = body.replace("color: #FFFFFF", "color: " + ON_ACCENT)
    return body


def light3(s):
    s = s.replace("theme/Adwaita/gtk-dark.css", "theme/Adwaita/gtk.css")
    s = s.replace("Tema GTK3 de Aurora.", "Tema GTK3 de Aurora (claro).")
    for name, val in GTK3_LIGHT_DEFS.items():
        s = re.sub(r"(@define-color %s )[^;]+;" % name, r"\g<1>%s;" % val, s)
    s = recolor_rules(s, light_body)
    return s


GTK4_LIGHT_DEFS = {
    "window_bg_color": "#F4F7F5", "window_fg_color": "#14211B",
    "view_bg_color": "#FFFFFF", "view_fg_color": "#14211B",
    "headerbar_bg_color": "#E6EBE9", "headerbar_fg_color": "#14211B",
    "headerbar_border_color": "rgba(0, 0, 0, 0.14)", "headerbar_backdrop_color": "#EEF2F0",
    "headerbar_shade_color": "rgba(0, 0, 0, 0.14)",
    "card_bg_color": "#FFFFFF", "card_fg_color": "#14211B", "card_shade_color": "rgba(0, 0, 0, 0.10)",
    "dialog_bg_color": "#F4F7F5", "dialog_fg_color": "#14211B",
    "popover_bg_color": "#FFFFFF", "popover_fg_color": "#14211B",
    "popover_shade_color": "rgba(0, 0, 0, 0.10)",
    "sidebar_bg_color": "#EDF1EF", "sidebar_fg_color": "#14211B",
    "sidebar_backdrop_color": "#F1F4F2", "sidebar_shade_color": "rgba(0, 0, 0, 0.10)",
    "secondary_sidebar_bg_color": "#F1F4F2", "secondary_sidebar_fg_color": "#14211B",
    "thumbnail_bg_color": "#E9EEEC", "thumbnail_fg_color": "#14211B",
    "accent_color": "#1C8A5C",
}


def light4(s):
    for name, val in GTK4_LIGHT_DEFS.items():
        s = re.sub(r"(@define-color %s )[^;]+;" % name, r"\g<1>%s;" % val, s)
    s = recolor_rules(s, light_body)
    return s


def main():
    # GTK3
    p3 = os.path.join(TH, "Aurora/gtk-3.0/gtk.css")
    d3 = dark(read(p3))
    write(p3, d3)
    write(os.path.join(TH, "Aurora-light/gtk-3.0/gtk.css"), light3(d3))
    # GTK4
    p4 = os.path.join(TH, "Aurora/gtk-4.0/aurora.css")
    d4 = dark(read(p4))
    write(p4, d4)
    write(os.path.join(TH, "Aurora-light/gtk-4.0/aurora.css"), light4(d4))
    write(os.path.join(TH, "Aurora-light/gtk-4.0/gtk.css"),
          '/* Tema GTK4 de Aurora (claro). */\n'
          '@import url("resource:///org/gtk/libgtk/theme/Default/Default-light.css");\n'
          '@import url("file:///usr/share/themes/Aurora-light/gtk-4.0/aurora.css");\n')
    idx = read(os.path.join(TH, "Aurora/index.theme"))
    write(os.path.join(TH, "Aurora-light/index.theme"),
          idx.replace("Name=Aurora", "Name=Aurora-light").replace("GtkTheme=Aurora", "GtkTheme=Aurora-light"))
    # libadwaita lee $XDG_CONFIG_HOME/gtk-4.0/gtk.css: una por modo
    write(os.path.join(XDG, "gtk-4.0/gtk-light.css"),
          '/* Apps libadwaita en modo claro. */\n'
          '@import url("file:///usr/share/themes/Aurora-light/gtk-4.0/aurora.css");\n')
    print("OK: Aurora (oscuro) y Aurora-light")


main()
