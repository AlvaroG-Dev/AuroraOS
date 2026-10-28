# Ratón "pillado" sobre el Dock / borde derecho

## Síntoma
En VM (PS/2 y USB, 1920x1080) el cursor se congela del todo al pasar por el Dock
o a veces en el borde derecho. Al moverlo a otro sitio vuelve a funcionar.

## Causa raíz
`simd_copy_to_vram` (kernel/simd.c) copia a VRAM con stores no temporales y
ejecutaba `_mm_sfence()` ANTES de la cola escalar (`for (; i < n; i++) dst[i] = src[i]`).
La VRAM está mapeada write-combining: esos stores normales también se quedan en
los buffers WC hasta que algo los desaloja. El compositor vuelca solo rectángulos
sucios mínimos (el cursor son unas decenas de píxeles por fila) y en esos casos
casi todo el trabajo es cabeza/cola escalar: el último trozo escrito no llegaba a
la pantalla hasta el siguiente volcado. En el borde derecho las filas recortadas
son siempre cola corta; en el Dock los rects son pequeños y repetidos.
No se ve en QEMU/TCG porque ahí los stores son inmediatos.

El clamp del cursor, el hit-test del Dock, el enrutado de eventos y los despertares
del compositor se revisaron y no tenían fallo.

## Arreglo
- `sfence` movido al final de las tres variantes `copy_nt_*`.
- `simd_copy_to_vram` hace `sfence` también tras la variante escalar.

## Verificación
- `make iso` + arranque en QEMU (TCG): 201 tests del kernel PASS, 0 FAIL, cursor
  responde. El síntoma original no es reproducible en QEMU/TCG; hay que confirmarlo
  en la VM del usuario.
