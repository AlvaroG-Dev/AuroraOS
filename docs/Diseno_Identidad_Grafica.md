# AuroraOS — Identidad gráfica (propuesta v0)

Estado: **propuesta**, pendiente de elegir alternativa (§3). Escrita tras revisar
`kernel/gfx/theme.h`, `dock.c`, `about.c` (logo), `bootloader/common/aurora_splash_core.h`,
`gen_wallpaper.py` y `kernel/gfx/fonts/`.

## 1. Punto de partida (lo que ya hay)

| Elemento | Hoy |
|---|---|
| Tema | `theme.h`: grises neutros + **un** acento `#0A84FF`; modo oscuro estilo macOS Tahoe, cristal (blur+saturación+borde especular), semáforos, sombra en dos capas. |
| Logo | Esfera con degradado cian `#64D2FF` → azul `#0A84FF` → índigo `#5E5CE6` y tres cintas blancas (la "aurora"), dibujada por código en `about.c`. |
| Splash | Wordmark vectorial "AuroraOS" letra a letra + spinner, fondo azul-negro (`#050810`→`#0A1022`), núcleo común BIOS/UEFI/kernel. |
| Fondo | `gen_wallpaper.py`: ondas azules procedurales. |
| Tipografía | Inter (UI) y JetBrains Mono (terminal), solo tamaño 14 pre-rasterizado. |

Diagnóstico: la base técnica es sólida y coherente, pero visualmente es "macOS en azul". Lo
único propio es la esfera con cintas, y apenas aparece fuera de *Acerca de*. Falta un
**rasgo firma** reconocible a primera vista.

## 2. Idea central: «la cinta de aurora»

Un único motivo recurrente: una **cinta de degradado teal → azul → violeta** (la aurora
boreal) que aparece solo en momentos de marca y nunca como relleno de UI general.

- Degradado firma `AURORA_RIBBON`: `#3DF2C4` (verde aurora) → `#0A84FF` (azul sistema) → `#8E5CF6` (violeta).
- El acento de UI sigue siendo **uno** (azul), para no ensuciar la sobriedad actual.
- Dónde se usa la cinta: logo, línea de progreso del splash, foco de la ventana activa
  (hilo de 1 px bajo la barra de título), indicador de app abierta en el Dock, barra de
  progreso, selección en el prompt del terminal y wallpaper por defecto.
- Coste: todo es interpolación de enteros sobre 2–3 colores; sin librerías ni texturas.

## 3. Alternativas

**A — Aurora Boreal (recomendada).** Cinta teal→azul→violeta como firma; UI gris neutro +
acento azul. Fondo: cielo casi negro azulado con cinta difusa. Es la más distintiva y la
más fiel al nombre.

**B — Aurora Índigo (conservadora).** Sin verde: cinta azul→índigo→magenta suave, acento
`#5E5CE6`. Más cercana al look actual y a macOS; menos reconocible, pero sin cambios
visibles en el resto de la UI.

## 4. Especificación (alternativa A)

**Paleta**

| Token | Valor | Uso |
|---|---|---|
| `AURORA_BRAND_TEAL` | `#3DF2C4` | extremo verde de la cinta |
| `AURORA_BRAND_BLUE` | `#0A84FF` | = acento de UI |
| `AURORA_BRAND_VIOLET` | `#8E5CF6` | extremo violeta de la cinta |
| Fondo marca | `#050810` → `#0A1022` | splash y wallpaper (ya en uso) |
| Texto marca | `#E8F0FF` | wordmark (ya en uso) |

**Tipografía.** Inter para UI, JetBrains Mono para terminal (ya integradas). Wordmark
"AuroraOS" en Inter Semibold con "Aurora" en peso medio y "OS" en peso ligero; ligero
espaciado (+2 %). Pendiente: rasterizar 12/13/17/22 px si se quiere jerarquía real.

**Logo.** Mantener la esfera, pero teñir las cintas con `AURORA_RIBBON` en vez de blanco
puro, y con un mínimo de 16 px de legibilidad: versión simplificada de **una** cinta para
tamaños ≤ 32 px (favicon, icono del Dock, ventana). Versión monocroma blanca para
sobre-fondos claros.

**Iconos.** Estilo "squircle" con degradado vertical suave de un solo matiz por app,
glifo blanco; las apps de sistema (Terminal, Archivos, Ajustes) usan matices distintos
dentro de la gama teal–azul–violeta para que el Dock quede armónico.

**Ventanas.** Se conservan radio 20, semáforos y sombra de dos capas. Novedad: hilo de
1 px con `AURORA_RIBBON` bajo la barra de título solo en la ventana con foco.

**Dock y barra de menús.** Cristal actual. El punto de app abierta pasa a ser un
degradado corto de la cinta; el logo ocupa el primer menú con su
versión de una cinta.

**Terminal.** Fondo `#0E121C` (azul-negro de marca, ligeramente translúcido), texto
`#E8F0FF`, cursor teal `#3DF2C4`, ANSI de 16 colores calibrado a la gama (rojo `#FF6B6B`,
verde `#3DF2C4`, amarillo `#FFD166`, azul `#5AB0FF`, magenta `#B48CFF`, cian `#64D2FF`).

**Animaciones.** Se conservan las actuales (abrir 240 ms, cerrar 170, genie 460). Añadir:
un único "barrido" de la cinta (800 ms, ease-out) al terminar el splash antes de revelar
el escritorio. Curva común: ease-out cúbica; nada por encima de 500 ms salvo el genie.

**Boot splash.** Ya comparte núcleo BIOS/UEFI/kernel, así que el cambio es solo de
constantes: spinner `ASP_COL_SPIN` pasa a recorrer la cinta (un color por paso de los
12) y el halo gana un tinte teal. Cero costes nuevos.

**Wallpaper por defecto.** Regenerar `gen_wallpaper.py` con paleta de cielo nocturno
y 1–2 cintas de aurora desenfocadas; conservar el azul actual como "Aurora Azul".

## 5. Plan de implementación

1. **Hecho en esta rama:** tokens de marca añadidos a `theme.h` (aditivo, sin cambio visual).
2. Usar `AURORA_RIBBON` en `about.c` (logo) y en el indicador del Dock.
3. Splash: spinner en cinta + barrido final (tocar `aurora_splash_core.h`, probar en QEMU BIOS y UEFI).
4. Terminal: paleta ANSI y color de cursor.
5. Wallpaper nuevo + iconos del Dock.
6. Rasterizar tamaños de fuente adicionales.

## 6. Decisiones para Alvaro

1. ¿Alternativa **A** (teal→azul→violeta) o **B** (azul→índigo→magenta)?
2. ¿Mantienes el nombre en `AuroraOS` camel-case o prefieres `Aurora`/`aurora`?
3. ¿Referencias que te gusten o que quieras evitar (macOS, GNOME, Windows 11, KDE...)?
4. ¿Modo claro además del oscuro? Hoy solo hay oscuro.
