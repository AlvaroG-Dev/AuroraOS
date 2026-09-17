// kernel/gfx/winsrv.c
#include "winsrv.h"
#include "compositor.h"
#include "../heap.h"
#include "../klog.h"
#include "../string.h"
#include "../tty.h"

// ---------------------------------------------------------------------------
// Tabla de ventanas de usuario
// ---------------------------------------------------------------------------
typedef struct {
    window_t *win;
    task_t   *owner;
    int       in_use;
    int       is_console;
    wait_queue_t event_wq;
    winsrv_event_t events[WINSRV_EVENT_QUEUE];
    int event_head, event_tail, event_count;
} winsrv_entry_t;

static winsrv_entry_t g_windows[WINSRV_MAX_WINDOWS];
static int g_console_win_id = -1;
static int g_ready = 0;

// ---------------------------------------------------------------------------
// Helpers internos
// ---------------------------------------------------------------------------
static int find_free_slot(void) {
    for (int i = 0; i < WINSRV_MAX_WINDOWS; i++) {
        if (!g_windows[i].in_use)
            return i;
    }
    return -1;
}

static int find_slot_by_win(window_t *win) {
    if (!win) return -1;
    for (int i = 0; i < WINSRV_MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].win == win)
            return i;
    }
    return -1;
}

static void enqueue_event(winsrv_entry_t *e, const winsrv_event_t *ev) {
    // Si la cola está llena, descartar el más viejo para no bloquear
    // al compositor.
    if (e->event_count >= WINSRV_EVENT_QUEUE) {
        e->event_head = (e->event_head + 1) % WINSRV_EVENT_QUEUE;
        e->event_count--;
    }
    e->events[e->event_tail] = *ev;
    e->event_tail = (e->event_tail + 1) % WINSRV_EVENT_QUEUE;
    e->event_count++;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void winsrv_init(void) {
    memset(g_windows, 0, sizeof(g_windows));
    for (int i = 0; i < WINSRV_MAX_WINDOWS; i++) {
        wait_queue_init(&g_windows[i].event_wq);
    }
    g_console_win_id = -1;
    g_ready = 1;
    LOG_INFO("[WINSRV] Inicializado (%d slots)", WINSRV_MAX_WINDOWS);
}

// ---------------------------------------------------------------------------
// Crear / destruir
// ---------------------------------------------------------------------------
int winsrv_create_window(task_t *owner, int x, int y, int w, int h,
                         const char *title) {
    if (!g_ready || !owner)
        return -1;

    // Validar dimensiones mínimas razonables.
    if (w < 64 || h < 64 || w > 2048 || h > 2048)
        return -1;

    // Copiar título a kernel (viene de userland).
    char ktitle[64];
    size_t i = 0;
    stac();
    while (title && title[i] && i < sizeof(ktitle) - 1) {
        ktitle[i] = title[i];
        i++;
    }
    clac();
    ktitle[i] = '\0';

    int slot = find_free_slot();
    if (slot < 0) {
        LOG_ERR("[WINSRV] no hay slots libres");
        return -1;
    }

    // Crear la ventana en el compositor.
    preempt_disable();
    window_t *win = compositor_create_window(x, y, w, h, ktitle, 0);
    if (!win) {
        preempt_enable();
        LOG_ERR("[WINSRV] compositor_create_window falló");
        return -1;
    }

    g_windows[slot].win = win;
    g_windows[slot].owner = owner;
    g_windows[slot].in_use = 1;
    g_windows[slot].is_console = 0;
    g_windows[slot].event_head = 0;
    g_windows[slot].event_tail = 0;
    g_windows[slot].event_count = 0;

    preempt_enable();

    LOG_INFO("[WINSRV] ventana %d creada para task %u (%dx%d '%s')",
             slot, owner->id, w, h, ktitle);
    return slot;
}

int winsrv_destroy_window(task_t *owner, int win_id) {
    if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS)
        return -1;
    winsrv_entry_t *e = &g_windows[win_id];
    if (!e->in_use || e->owner != owner)
        return -1;

    // Si era la consola, desregistrar.
    if (g_console_win_id == win_id)
        g_console_win_id = -1;

    window_t *win = e->win;

    preempt_disable();
    e->in_use = 0;
    e->win = NULL;
    e->owner = NULL;
    e->is_console = 0;
    preempt_enable();

    if (win) {
        compositor_close_window(win);
    }

    LOG_INFO("[WINSRV] ventana %d destruida", win_id);
    return 0;
}

// ---------------------------------------------------------------------------
// Blit
// ---------------------------------------------------------------------------
int winsrv_blit(task_t *owner, int win_id, int x, int y, int w, int h,
                const uint32_t *user_pixels) {
    if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS)
        return -1;
    winsrv_entry_t *e = &g_windows[win_id];
    if (!e->in_use || e->owner != owner || !e->win)
        return -1;
    if (!user_pixels || w <= 0 || h <= 0)
        return -1;

    window_t *win = e->win;
    if (!win->content_buffer)
        return -1;

    // Recortar al área del content_buffer.
    int src_x = 0, src_y = 0;
    int dst_x = x, dst_y = y;
    int copy_w = w, copy_h = h;

    if (dst_x < 0) { src_x = -dst_x; copy_w -= src_x; dst_x = 0; }
    if (dst_y < 0) { src_y = -dst_y; copy_h -= src_y; dst_y = 0; }
    if (dst_x + copy_w > win->content_w) copy_w = win->content_w - dst_x;
    if (dst_y + copy_h > win->content_h) copy_h = win->content_h - dst_y;
    if (copy_w <= 0 || copy_h <= 0)
        return 0;   // nada que copiar, no es error

    // Copiar fila a fila desde userland.
    preempt_disable();
    stac();
    for (int row = 0; row < copy_h; row++) {
        const uint32_t *src = user_pixels + (src_y + row) * w + src_x;
        uint32_t *dst = win->content_buffer + (dst_y + row) * win->content_w + dst_x;
        for (int col = 0; col < copy_w; col++) {
            dst[col] = src[col];
        }
    }
    clac();
    win->dirty = 1;
    preempt_enable();

    // Notificar al compositor que hay daño.
    compositor_invalidate_rect((rect_t){
        win->x + dst_x,
        win->y + WIN11_TITLEBAR_HEIGHT + dst_y,
        copy_w,
        copy_h
    });

    return 0;
}

// ---------------------------------------------------------------------------
// Poll event
// ---------------------------------------------------------------------------
static bool winsrv_has_events(void *arg) {
    winsrv_entry_t *e = (winsrv_entry_t *)arg;
    return e->event_count > 0;
}

int winsrv_poll_event(task_t *owner, int win_id, winsrv_event_t *out_user_ev,
                      int blocking) {
    if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS || !out_user_ev)
        return -1;
    winsrv_entry_t *e = &g_windows[win_id];
    if (!e->in_use || e->owner != owner)
        return -1;

    if (e->event_count == 0) {
        if (!blocking)
            return 0;
        int rc = wait_event_interruptible(&e->event_wq, winsrv_has_events, e);
        if (rc < 0)
            return -1;
        // Al volver de la wq, la condición es cierta. Sacar el evento.
    }

    winsrv_event_t ev = e->events[e->event_head];
    e->event_head = (e->event_head + 1) % WINSRV_EVENT_QUEUE;
    e->event_count--;

    stac();
    *out_user_ev = ev;
    clac();
    return 1;
}

// ---------------------------------------------------------------------------
// Consola
// ---------------------------------------------------------------------------
int winsrv_register_console(task_t *owner, int win_id) {
    if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS)
        return -1;
    winsrv_entry_t *e = &g_windows[win_id];
    if (!e->in_use || e->owner != owner)
        return -1;

    preempt_disable();
    // Desregistrar la consola previa si había otra.
    if (g_console_win_id >= 0 && g_console_win_id != win_id) {
        g_windows[g_console_win_id].is_console = 0;
    }
    g_console_win_id = win_id;
    e->is_console = 1;
    preempt_enable();

    LOG_INFO("[WINSRV] ventana %d registrada como consola", win_id);
    return 0;
}

// ---------------------------------------------------------------------------
// API interna para el compositor y el VFS
// ---------------------------------------------------------------------------
void winsrv_post_event(window_t *win, uint32_t type, int32_t x, int32_t y,
                       uint32_t data) {
    if (!g_ready || !win)
        return;
    int slot = find_slot_by_win(win);
    if (slot < 0)
        return;

    winsrv_entry_t *e = &g_windows[slot];
    winsrv_event_t ev = {
        .type = type,
        .x = x,
        .y = y,
        .data = data,
    };
    enqueue_event(e, &ev);
    wake_up_all(&e->event_wq);
}

void winsrv_console_output(char c) {
    if (!g_ready || g_console_win_id < 0)
        return;
    winsrv_entry_t *e = &g_windows[g_console_win_id];
    if (!e->in_use || !e->win)
        return;

    // Enviamos cada byte como un evento OUTPUT. La app lo acumula
    // internamente y hace un solo blit por frame.
    winsrv_post_event(e->win, WINSRV_EV_OUTPUT, (int32_t)(uint8_t)c, 0, 0);
}
