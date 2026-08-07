// kernel/gfx/font_manager.h
#pragma once
#include "font_aa.h"
#include "gfx.h"

// Inicializa las tablas de fuentes en memoria
void font_manager_init(void);

// Obtiene la estructura de fuente asociada a un ID
const font_aa_t *font_manager_get(font_id_t id);