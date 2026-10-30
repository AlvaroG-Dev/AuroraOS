# AuroraOS — Identidad gráfica (v2, decisiones de Alvaro aplicadas)

**Decidido:** verde intermedio entre A y C = `#54DA75` (hover `#7BE594`, texto sobre acento `#08260F`); semáforos conservados pero con colores propios: cerrar rosa `#FF5C8A`, minimizar ámbar `#FFB547`, maximizar turquesa `#3CC8E0`; habrá modo claro (pendiente de diseñar). Lo que sigue es el razonamiento original; donde un valor difiera, manda esta línea.

Sin degradados como seña de identidad: **un color sólido de marca** sobre la base de
grises neutros que ya existe (`kernel/gfx/theme.h`). Hoy el acento es `#0A84FF`, el azul
de macOS, y los semáforos de ventana (`#FF5F57/#FEBC2E/#28C840`) son los de macOS.

## 1. Por qué verde (psicología del color)

- **Azul**: confianza y calma, pero es el color de macOS, Windows, Ubuntu-Unity, Facebook...
  Es el menos diferenciador del mercado de sistemas operativos.
- **Rojo/naranja**: energía, urgencia; Ubuntu y Fedora ya lo ocupan y cansa en uso largo.
- **Morado/índigo**: creativo y premium, pero poco asociado a "sistema fiable" y muy usado en apps.
- **Verde**: asociado a naturaleza, equilibrio, "listo / correcto" y descanso visual (el ojo
  distingue más matices de verde que de cualquier otro tono). Encaja con el nombre
  (aurora boreal verde, el color más común de la aurora) y con terminales clásicos.
  Lo usan pocos sistemas de escritorio: Linux Mint, Android (logo) y poco más, ninguno en
  modo oscuro con grises neutros. Riesgo: confundirlo con "éxito"; se evita no usándolo
  además como indicador de OK en la UI, ver §3.

## 2. Candidatos (todos sólidos, pensados para fondo oscuro)

| | Nombre | Hex | Carácter |
|---|---|---|---|
| **A (recomendado)** | Verde aurora | `#2DD4A0` | Verde-menta vivo; se lee muy bien sobre gris oscuro, fresco y moderno. |
| B | Verde esmeralda | `#10B981` | Más clásico y sobrio; menos luminoso. |
| C | Verde lima | `#7BE04A` | Muy energético y "hacker"; cansa más y choca con el cristal. |

Derivados del elegido (A): hover `#5BE3B8`, tenue (selección inactiva) `#2DD4A0` al 33 %,
texto sobre acento **oscuro** `#06241A` (el blanco sobre verde claro no llega a contraste 4.5:1).

## 3. Botones de ventana y estados

Los semáforos rojo/amarillo/verde son la seña más reconocible de macOS. Opciones:

1. **Monocromo de marca (recomendado):** los tres botones son círculos grises
   (`#4A4A4D`) y al pasar el ratón por el grupo se tiñen: cerrar `#FF6B6B` coral,
   minimizar `#F2C14E` ámbar, maximizar el verde aurora. Misma ergonomía, aspecto propio.
2. Botones a la derecha, rectángulos redondeados con glifo (estilo propio, más cercano a Windows).
3. Conservar los semáforos y cambiar solo el acento.

Estados: error `#FF6B6B`, aviso `#F2C14E`, info `#5AB0FF`; el verde de marca es el
único acento, no se usa "verde" para significar "correcto" (para eso, un check con
glifo neutro).

## 4. Resto del sistema (sin degradados de marca)

- **Superficies:** grises neutros actuales; cristal en Dock y menús sin cambios.
- **Logo:** esfera/disco sólido verde aurora con las tres cintas blancas (quitar el
  degradado cian→índigo de `about.c`). Versión monocroma blanca sobre fondo claro.
- **Dock:** punto de app abierta y foco = verde aurora, sin degradado.
- **Terminal:** fondo `#0E1412` (negro con un matiz verde casi imperceptible), texto
  `#E6EFEA`, cursor `#2DD4A0`; ANSI: rojo `#FF6B6B`, verde `#2DD4A0`, amarillo `#F2C14E`,
  azul `#5AB0FF`, magenta `#C792EA`, cian `#5EEAD4`.
- **Splash:** fondo negro-verdoso muy oscuro (`#07100D`), wordmark `#E6EFEA`, spinner verde
  aurora (`ASP_COL_SPIN`, `ASP_TOP/BOT_*` en `aurora_splash_core.h`).
- **Wallpaper:** noche con una única cinta de aurora verde difuminada; conservar el azul
  actual como "Aurora Azul" alternativo.
- **Tipografía:** Inter (UI) y JetBrains Mono (terminal); sin cambios.
- **Animaciones:** las actuales; sin añadidos.

## 5. Implementación

1. **En esta rama:** `AURORA_ACCENT` pasa a verde aurora (`theme.h`) más tokens derivados.
   Todo lo que usa el acento (selección de menús, Dock, orbe de la barra de menús) cambia solo.
2. Pendiente de decisión: botones de ventana (§3), logo sin degradado, splash, terminal, wallpaper.
3. Detalle a revisar al probar: texto blanco sobre el acento en la selección de menús
   (`AURORA_MENU_HILITE`) pasará a texto oscuro.

## 6. Decisiones para Alvaro

1. ¿Verde A, B o C?
2. ¿Botones de ventana: opción 1, 2 o 3?
3. ¿Modo claro además del oscuro?
