// user/lib/font_system.h
#pragma once
#include "font_aa.h"

// Fuente del sistema: JetBrains Mono Regular 14. Misma que el kernel
// usa como FONT_ID_MONO en font_manager. Devuelve un puntero a la
// tabla, que vive en .rodata del binario.
const font_aa_t *font_system_mono(void);