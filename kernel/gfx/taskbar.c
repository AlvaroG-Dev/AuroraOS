#include "taskbar.h"
#include "theme.h"
#include "font_manager.h"
#include "font_aa.h"
#include "../heap.h"
#include "rtc.h"

// Variables estáticas internas de la taskbar
static taskbar_item_t *taskbar_items = NULL;
static uint32_t next_item_id = 1;
static char clock_time_buffer[6]; // HH:MM + nullo
static char clock_date_buffer[11]; // DD/MM/YYYY + nullo

typedef struct {
    int advance_w;
    int ink_x0;
    int ink_y0;
    int ink_x1;
    int ink_y1;
    int ink_w;
    int ink_h;
} taskbar_text_metrics_t;

typedef struct {
    const font_aa_t *icon_font;
    const font_aa_t *label_font;
    const font_aa_t *clock_font;
} taskbar_fonts_t;

typedef struct {
    taskbar_text_metrics_t icon_metrics;
    taskbar_text_metrics_t label_metrics;
    int has_start_icon;
    int has_symbol;
    int has_label;
    int icon_w;
    int icon_h;
    int label_w;
    int label_h;
    int content_w;
    int content_h;
    int item_w;
} taskbar_item_layout_t;

static int taskbar_max(int a, int b) {
    return (a > b) ? a : b;
}

// Actualiza el reloj leyendo del RTC y formatea hora y fecha.
// Forward declarations for functions used in taskbar_hit_test
static taskbar_fonts_t taskbar_get_fonts(void);
static int taskbar_items_block_width(taskbar_fonts_t fonts, int *out_count);
static int taskbar_clock_area_width(taskbar_fonts_t fonts);
static taskbar_item_layout_t taskbar_measure_item(taskbar_item_t *item, taskbar_fonts_t fonts);

void taskbar_refresh_clock(void) {
    rtc_datetime_t now;
    if (!rtc_read_datetime(&now)) {
        // Si falla, dejamos los buffers como estaban o ponemos algo por defecto.
        return;
    }
    rtc_format_time(&now, clock_time_buffer, sizeof(clock_time_buffer));
    rtc_format_date(&now, clock_date_buffer, sizeof(clock_date_buffer));
}

taskbar_item_t *taskbar_add_item_bmp(taskbar_item_type_t type, const char *label, tar_node_t *bmp_file, void (*on_click)(taskbar_item_t*)) {
    // Si ya existe un ítem del tipo START_BTN, lo actualizamos en lugar de duplicarlo
    if (type == TASKBAR_ITEM_START_BTN && taskbar_items) {
        taskbar_item_t *curr = taskbar_items;
        while (curr) {
            if (curr->type == TASKBAR_ITEM_START_BTN) {
                curr->icon_bmp_node = bmp_file;
                if (on_click) curr->on_click = on_click;
                int i = 0;
                while (label && label[i] && i < 31) { curr->label[i] = label[i]; i++; }
                curr->label[i] = '\0';
                return curr;
            }
            curr = curr->next;
        }
    }

    taskbar_item_t *item = taskbar_add_item(type, label, "", 0, on_click);
    if (item) {
        item->icon_bmp_node = bmp_file;
    }
    return item;
}

// Establece el ítem activo de la taskbar (por puntero)
void taskbar_set_active_item(taskbar_item_t *item) {
    // Desactivar todos los ítems
    taskbar_item_t *curr = taskbar_items;
    while (curr) {
        curr->is_active = 0;
        curr = curr->next;
    }
    // Activar el ítem proporcionado (si no es NULL)
    if (item) {
        item->is_active = 1;
    }
    // Dañar el área de la taskbar para que se vuelva a dibujar
    // Nota: Esta función se llama desde el hilo del compositor, así que podemos
    // invalidar directamente el área de la taskbar.
    // Usamos las variables externas del compositor (fb_width, fb_height) mediante
    // una llamada a taskbar_get_bounds y luego invalidamos un rectángulo ligeramente
    // mayor para incluir el efecto de sombra.
    // Sin embargo, no tenemos acceso a fb_width y fb_height aquí. En su lugar,
    // dejaremos que el llamador (compositor_focus_window) se encargue de invalidar.
    // Alternativamente, podemos hacer que esta función devuelva un rectángulo de daño,
    // pero para simplificar, asumiremos que el llamador ya sabe que debe invalidar.
    // En el plan, el llamador invalidará la pantalla/taskbar.
}

// Busca el ítem de taskbar bajo las coordenadas (x, y) relativas a la pantalla.
// Devuelve NULL si no hay ningún ítem en esa posición.
taskbar_item_t *taskbar_hit_test(int screen_x, int screen_y, int screen_w, int screen_h) {
    // Usamos las mismas fuentes y métricas que en el render para lograr coherencia.
    taskbar_fonts_t fonts = taskbar_get_fonts();
    if (!fonts.icon_font || !fonts.label_font || !fonts.clock_font) return NULL;

    rect_t bar_rect = taskbar_get_bounds(screen_w, screen_h);
    int item_y = bar_rect.y + (TASKBAR_BAR_H - TASKBAR_ITEM_H) / 2;

    // Si el punto está fuera de la barra verticalmente, no hay hit.
    if (screen_y < item_y || screen_y >= item_y + TASKBAR_ITEM_H) {
        return NULL;
    }

    int item_count = 0;
    int items_w = taskbar_items_block_width(fonts, &item_count);
    int clock_area_w = taskbar_clock_area_width(fonts);
    int clock_area_x = screen_w - TASKBAR_BAR_PADDING_X - clock_area_w;
    int centered_x = (screen_w - items_w) / 2;
    int max_items_x = clock_area_x - TASKBAR_CLOCK_GAP - items_w;
    int current_x = centered_x;

    if (current_x < TASKBAR_BAR_PADDING_X) current_x = TASKBAR_BAR_PADDING_X;
    if (item_count > 0 && current_x > max_items_x) current_x = max_items_x;
    if (current_x < TASKBAR_BAR_PADDING_X) current_x = TASKBAR_BAR_PADDING_X;

    taskbar_item_t *curr = taskbar_items;
    while (curr) {
        taskbar_item_layout_t layout = taskbar_measure_item(curr, fonts);
        rect_t item_rect = {current_x, item_y, layout.item_w, TASKBAR_ITEM_H};
        if (screen_x >= item_rect.x && screen_x < item_rect.x + item_rect.w) {
            // Hit! El punto está dentro del rectángulo del ítem (en Y ya lo comprobamos).
            return curr;
        }
        current_x += layout.item_w + TASKBAR_ITEM_GAP;
        curr = curr->next;
    }

    // También podríamos comprobar el área del reloj, pero no tiene ítems asociados.
    return NULL;
}

static int taskbar_min(int a, int b) {
    return (a < b) ? a : b;
}

static taskbar_fonts_t taskbar_get_fonts(void) {
    taskbar_fonts_t fonts;
    fonts.icon_font = font_manager_get(FONT_ID_MONO_BOLD);
    fonts.label_font = font_manager_get(FONT_ID_MAIN_BOLD);
    fonts.clock_font = font_manager_get(FONT_ID_MONO_BOLD);

    if (!fonts.label_font) fonts.label_font = fonts.icon_font;
    if (!fonts.clock_font) fonts.clock_font = fonts.icon_font;
    if (!fonts.icon_font) fonts.icon_font = fonts.label_font;
    return fonts;
}

/**
 * Calcula la caja visible real de una cadena con la fuente AA usada.
 * El advance se conserva para contenido sin píxeles visibles (p.ej. espacios),
 * pero el centrado usa ink_* para que bearing_x/bearing_y no desplacen el texto.
 */
static taskbar_text_metrics_t taskbar_measure_text(const font_aa_t *font, const char *text) {
    taskbar_text_metrics_t m = {0, 0, 0, 0, 0, 0, 0};
    if (!font || !text || !text[0]) return m;

    int cur_x = 0;
    int has_ink = 0;
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;

    while (*text) {
        unsigned char c = (unsigned char)*text;
        if (c < 128) {
            const glyph_aa_t *g = &font->glyphs[c];
            if (g->bitmap && g->width > 0 && g->height > 0) {
                int gx0 = cur_x + g->bearing_x;
                int gy0 = font->height - g->bearing_y;
                int gx1 = gx0 + g->width;
                int gy1 = gy0 + g->height;

                if (!has_ink) {
                    min_x = gx0;
                    min_y = gy0;
                    max_x = gx1;
                    max_y = gy1;
                    has_ink = 1;
                } else {
                    min_x = taskbar_min(min_x, gx0);
                    min_y = taskbar_min(min_y, gy0);
                    max_x = taskbar_max(max_x, gx1);
                    max_y = taskbar_max(max_y, gy1);
                }
            }
            cur_x += g->advance;
        }
        text++;
    }

    m.advance_w = cur_x;
    if (has_ink) {
        m.ink_x0 = min_x;
        m.ink_y0 = min_y;
        m.ink_x1 = max_x;
        m.ink_y1 = max_y;
        m.ink_w = max_x - min_x;
        m.ink_h = max_y - min_y;
    } else if (cur_x > 0) {
        m.ink_x0 = 0;
        m.ink_y0 = 0;
        m.ink_x1 = cur_x;
        m.ink_y1 = font->height;
        m.ink_w = cur_x;
        m.ink_h = font->height;
    }

    return m;
}

static int taskbar_metric_width(taskbar_text_metrics_t m) {
    return (m.ink_w > 0) ? m.ink_w : m.advance_w;
}

static int taskbar_text_width(const font_aa_t *font, const char *text) {
    return taskbar_metric_width(taskbar_measure_text(font, text));
}

static void taskbar_draw_text_centered(uint32_t *dst, int stride, rect_t clip,
                                       int center_x, int center_y, const char *text,
                                       uint32_t color, font_id_t font_id,
                                       const font_aa_t *font,
                                       taskbar_text_metrics_t metrics) {
    if (!text || !text[0] || !font) return;

    int tx = center_x - (metrics.ink_x0 + metrics.ink_x1) / 2;
    int ty = center_y - (metrics.ink_y0 + metrics.ink_y1) / 2;

    // Sombra de 1px para que iconos/textos finos no se pierdan sobre el Mica.
    gfx_draw_string(dst, stride, clip, tx, ty + 1, text, 0x78000000, font_id);
    gfx_draw_string(dst, stride, clip, tx, ty, text, color, font_id);
}

static void taskbar_draw_start_icon(uint32_t *dst, int stride, rect_t clip,
                                    int left, int center_y, uint32_t color) {
    int top = center_y - TASKBAR_START_ICON_SIZE / 2;
    int sq = TASKBAR_START_ICON_SQ;
    int gap = TASKBAR_START_ICON_GAP;

    gfx_fill_rounded_rect(dst, stride, clip, (rect_t){left, top, sq, sq}, 2, color);
    gfx_fill_rounded_rect(dst, stride, clip, (rect_t){left + sq + gap, top, sq, sq}, 2, color);
    gfx_fill_rounded_rect(dst, stride, clip, (rect_t){left, top + sq + gap, sq, sq}, 2, color);
    gfx_fill_rounded_rect(dst, stride, clip, (rect_t){left + sq + gap, top + sq + gap, sq, sq}, 2, color);
}

static taskbar_item_layout_t taskbar_measure_item(taskbar_item_t *item, taskbar_fonts_t fonts) {
    taskbar_item_layout_t layout;
    layout.icon_metrics = (taskbar_text_metrics_t){0, 0, 0, 0, 0, 0, 0};
    layout.label_metrics = (taskbar_text_metrics_t){0, 0, 0, 0, 0, 0, 0};
    layout.has_start_icon = 0;
    layout.has_symbol = 0;
    layout.has_label = 0;
    layout.icon_w = 0;
    layout.icon_h = 0;
    layout.label_w = 0;
    layout.label_h = 0;
    layout.content_w = 0;
    layout.content_h = 0;
    layout.item_w = TASKBAR_ITEM_MIN_W;

    if (!item) return layout;

    // Prioridad: Icono BMP -> Icono vectorial Start -> Símbolo de texto
    if (item->icon_bmp_node) {
        layout.icon_w = 20;
        layout.icon_h = 20;
    } else if (item->type == TASKBAR_ITEM_START_BTN) {
        layout.has_start_icon = 1;
        layout.icon_w = TASKBAR_START_ICON_SIZE;
        layout.icon_h = TASKBAR_START_ICON_SIZE;
    } else if (item->icon_symbol[0]) {
        layout.has_symbol = 1;
        layout.icon_metrics = taskbar_measure_text(fonts.icon_font, item->icon_symbol);
        layout.icon_w = taskbar_metric_width(layout.icon_metrics);
        layout.icon_h = layout.icon_metrics.ink_h;
    }

    if (item->label[0]) {
        layout.has_label = 1;
        layout.label_metrics = taskbar_measure_text(fonts.label_font, item->label);
        layout.label_w = taskbar_metric_width(layout.label_metrics);
        layout.label_h = layout.label_metrics.ink_h;
    }

    if (layout.icon_w > 0) layout.content_w += layout.icon_w;
    if (layout.label_w > 0) {
        if (layout.content_w > 0) layout.content_w += TASKBAR_CONTENT_GAP;
        layout.content_w += layout.label_w;
    }

    layout.content_h = taskbar_max(layout.icon_h, layout.label_h);
    if (layout.content_w <= 0) {
        layout.content_w = TASKBAR_START_ICON_SIZE;
        layout.content_h = TASKBAR_START_ICON_SIZE;
    }

    if (item->width > 0) {
        layout.item_w = item->width;
    } else {
        layout.item_w = layout.content_w + TASKBAR_ITEM_PADDING_X * 2;
        layout.item_w = taskbar_max(layout.item_w, TASKBAR_ITEM_MIN_W);
    }

    return layout;
}

static int taskbar_items_block_width(taskbar_fonts_t fonts, int *out_count) {
    int width = 0;
    int count = 0;
    taskbar_item_t *curr = taskbar_items;

    while (curr) {
        taskbar_item_layout_t layout = taskbar_measure_item(curr, fonts);
        if (count > 0) width += TASKBAR_ITEM_GAP;
        width += layout.item_w;
        count++;
        curr = curr->next;
    }

    if (out_count) *out_count = count;
    return width;
}

static int taskbar_clock_area_width(taskbar_fonts_t fonts) {
    int time_w = taskbar_text_width(fonts.clock_font, clock_time_buffer);
    int date_w = taskbar_text_width(fonts.clock_font, clock_date_buffer);
    int clock_w = taskbar_max(time_w, date_w);
    return taskbar_max(TASKBAR_CLOCK_MIN_W, clock_w + TASKBAR_CLOCK_PADDING_X * 2);
}

rect_t taskbar_get_bounds(int screen_w, int screen_h) {
    int y = screen_h - TASKBAR_BAR_H - TASKBAR_BAR_MARGIN_BOTTOM;
    return (rect_t){0, y, screen_w, TASKBAR_BAR_H};
}

void taskbar_init(void) {
    taskbar_items = NULL;
    // Botón de Inicio con ancho automático por defecto (width = 0)
    taskbar_add_item(TASKBAR_ITEM_START_BTN, "Start", "win", 0xFF60CDFF, NULL);
    // Inicializar buffers de reloj
    clock_time_buffer[0] = '\0';
    clock_date_buffer[0] = '\0';
    taskbar_refresh_clock();
}

taskbar_item_t *taskbar_add_item_custom_width(taskbar_item_type_t type, const char *label, const char *symbol, uint32_t color, int explicit_width, void (*on_click)(taskbar_item_t*)) {
    taskbar_item_t *item = (taskbar_item_t *)kmalloc(sizeof(taskbar_item_t));
    if (!item) return NULL;

    item->id = next_item_id++;
    item->type = type;
    item->icon_color = color;
    item->is_active = 0;
    item->width = explicit_width; // 0 para automático, >0 para explícito
    item->on_click = on_click;
    item->next = NULL;

    int i = 0;
    while (label && label[i] && i < 31) { item->label[i] = label[i]; i++; }
    item->label[i] = '\0';

    i = 0;
    while (symbol && symbol[i] && i < 7) { item->icon_symbol[i] = symbol[i]; i++; }
    item->icon_symbol[i] = '\0';

    if (!taskbar_items) {
        taskbar_items = item;
    } else {
        taskbar_item_t *curr = taskbar_items;
        while (curr->next) curr = curr->next;
        curr->next = item;
    }
    return item;
}

taskbar_item_t *taskbar_add_item(taskbar_item_type_t type, const char *label, const char *symbol, uint32_t color, void (*on_click)(taskbar_item_t*)) {
    return taskbar_add_item_custom_width(type, label, symbol, color, 0, on_click); // width = 0 activa auto-sizing y auto-centering
}

void taskbar_remove_item(uint32_t id) {
    taskbar_item_t *curr = taskbar_items;
    taskbar_item_t *prev = NULL;
    while (curr) {
        if (curr->id == id) {
            if (prev) prev->next = curr->next;
            else taskbar_items = curr->next;
            // Free the item
            kfree(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

void taskbar_render(uint32_t *dst, int stride, rect_t clip, int screen_w, int screen_h) {
    taskbar_fonts_t fonts = taskbar_get_fonts();
    if (!fonts.icon_font || !fonts.label_font || !fonts.clock_font) return;

    rect_t bar_rect = taskbar_get_bounds(screen_w, screen_h);
    int item_y = bar_rect.y + (TASKBAR_BAR_H - TASKBAR_ITEM_H) / 2;

    gfx_blend_rect(dst, stride, clip, bar_rect, 0xEE202026);
    gfx_blend_rect(dst, stride, clip, (rect_t){0, bar_rect.y, screen_w, 1}, 0x30FFFFFF);
    gfx_blend_rect(dst, stride, clip, (rect_t){0, bar_rect.y + 1, screen_w, 1}, 0x12000000);

    int item_count = 0;
    int items_w = taskbar_items_block_width(fonts, &item_count);
    int clock_area_w = taskbar_clock_area_width(fonts);
    int clock_area_x = screen_w - TASKBAR_BAR_PADDING_X - clock_area_w;
    int centered_x = (screen_w - items_w) / 2;
    int max_items_x = clock_area_x - TASKBAR_CLOCK_GAP - items_w;
    int current_x = centered_x;

    if (current_x < TASKBAR_BAR_PADDING_X) current_x = TASKBAR_BAR_PADDING_X;
    if (item_count > 0 && current_x > max_items_x) current_x = max_items_x;
    if (current_x < TASKBAR_BAR_PADDING_X) current_x = TASKBAR_BAR_PADDING_X;

    taskbar_item_t *curr = taskbar_items;

    while (curr) {
        taskbar_item_layout_t layout = taskbar_measure_item(curr, fonts);
        rect_t item_rect = {current_x, item_y, layout.item_w, TASKBAR_ITEM_H};
        int center_x = current_x + layout.item_w / 2;
        int center_y = item_y + TASKBAR_ITEM_H / 2;

        if (curr->is_active) {
            gfx_fill_rounded_rect(dst, stride, clip, item_rect, TASKBAR_ITEM_RADIUS, 0x24FFFFFF);
            gfx_draw_rounded_border(dst, stride, clip, item_rect, TASKBAR_ITEM_RADIUS, 0x18FFFFFF);
        } else {
            gfx_fill_rounded_rect(dst, stride, clip, item_rect, TASKBAR_ITEM_RADIUS, 0x06FFFFFF);
        }

        int content_x = current_x + (layout.item_w - layout.content_w) / 2;

        // 1. Dibujar icono (BMP si está disponible, o vectorial Start, o Texto)
        if (curr->icon_bmp_node) {
            bmp_draw_scaled(curr->icon_bmp_node, dst, stride, clip, content_x, center_y - 10, 20, 20);
            content_x += 20;
            if (layout.has_label && layout.label_w > 0) content_x += TASKBAR_CONTENT_GAP;
        } else if (layout.has_start_icon) {
            taskbar_draw_start_icon(dst, stride, clip, content_x, center_y, curr->icon_color);
            content_x += TASKBAR_START_ICON_SIZE;
            if (layout.has_label && layout.label_w > 0) content_x += TASKBAR_CONTENT_GAP;
        } else if (layout.has_symbol && layout.icon_w > 0) {
            taskbar_draw_text_centered(dst, stride, clip,
                                       content_x + layout.icon_w / 2, center_y,
                                       curr->icon_symbol, curr->icon_color,
                                       FONT_ID_MONO_BOLD, fonts.icon_font,
                                       layout.icon_metrics);
            content_x += layout.icon_w;
            if (layout.has_label && layout.label_w > 0) content_x += TASKBAR_CONTENT_GAP;
        }

        // 2. Dibujar texto de la etiqueta
        if (layout.has_label && layout.label_w > 0) {
            taskbar_draw_text_centered(dst, stride, clip,
                                       content_x + layout.label_w / 2, center_y,
                                       curr->label, 0xFFF4F4F6,
                                       FONT_ID_MAIN_BOLD, fonts.label_font,
                                       layout.label_metrics);
        }

        // 3. Indicador inferior para app activa
        if (curr->is_active) {
            rect_t indicator = {center_x - 8, bar_rect.y + TASKBAR_BAR_H - 4, 16, 3};
            gfx_fill_rounded_rect(dst, stride, clip, indicator, 2, WIN11_ACCENT);
        }

        current_x += layout.item_w + TASKBAR_ITEM_GAP;
        curr = curr->next;
    }

    // Dibujar área de reloj...
    gfx_blend_rect(dst, stride, clip,
                   (rect_t){clock_area_x - TASKBAR_CLOCK_GAP / 2, bar_rect.y + 9,
                            TASKBAR_SEPARATOR_W, TASKBAR_BAR_H - 18},
                   0x22FFFFFF);

    rect_t clock_rect = {clock_area_x, item_y, clock_area_w, TASKBAR_ITEM_H};
    gfx_fill_rounded_rect(dst, stride, clip, clock_rect, TASKBAR_ITEM_RADIUS, 0x04FFFFFF);

    taskbar_text_metrics_t time_metrics = taskbar_measure_text(fonts.clock_font, clock_time_buffer);
    taskbar_text_metrics_t date_metrics = taskbar_measure_text(fonts.clock_font, clock_date_buffer);
    int center_x = clock_area_x + clock_area_w / 2;
    int time_y = bar_rect.y + TASKBAR_BAR_H / 2 - 6;
    int date_y = bar_rect.y + TASKBAR_BAR_H / 2 + 6;
    taskbar_draw_text_centered(dst, stride, clip, center_x, time_y, clock_time_buffer, 0xFFD7D7DE, FONT_ID_MONO_BOLD, fonts.clock_font, time_metrics);
    taskbar_draw_text_centered(dst, stride, clip, center_x, date_y, clock_date_buffer, 0xFFD7D7DE, FONT_ID_MONO_BOLD, fonts.clock_font, date_metrics);
}