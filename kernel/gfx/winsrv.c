// kernel/gfx/winsrv.c
#include "winsrv.h"
#include "../cpu.h"
#include "../heap.h"
#include "../klog.h"
#include "../spinlock.h"
#include "../string.h"
#include "../tty.h"
#include "../uaccess.h"
#include "../vfs.h"
#include "compositor.h"
#include "theme.h"

// ---------------------------------------------------------------------------
// Tabla de ventanas de usuario
// ---------------------------------------------------------------------------
typedef struct {
  window_t *win;
  task_t *owner;
  int in_use;
  int is_console;
  wait_queue_t event_wq;
  winsrv_event_t events[WINSRV_EVENT_QUEUE];
  int event_head, event_tail, event_count;
} winsrv_entry_t;

static winsrv_entry_t g_windows[WINSRV_MAX_WINDOWS];
static int g_console_win_id = -1;
static int g_ready = 0;

// ---------------------------------------------------------------------------
// [Fase B] Lock único que protege TODA la tabla g_windows[] y
// g_console_win_id. La wq de cada ventana tiene su propio lock y se
// usa FUERA de winsrv_lock para evitar deadlocks.
//
// Contrato de locking:
//   - Toda lectura/escritura de g_windows[i].{in_use,win,owner,is_console,
//     event_head,event_tail,event_count} y de g_console_win_id debe
//     hacerse con winsrv_lock cogido.
//   - La wq (event_wq) tiene su propio lock, gestionado por wait.c.
//     Las llamadas a wake_up_all/wait_event_* se hacen SIEMPRE fuera
//     de winsrv_lock.
// ---------------------------------------------------------------------------
static spinlock_t winsrv_lock;

// ---------------------------------------------------------------------------
// Helpers internos. IMPORTANTE: deben llamarse con winsrv_lock cogido.
// ---------------------------------------------------------------------------
static int find_free_slot_locked(void) {
  for (int i = 0; i < WINSRV_MAX_WINDOWS; i++) {
    if (!g_windows[i].in_use)
      return i;
  }
  return -1;
}

static int find_slot_by_win_locked(window_t *win) {
  if (!win)
    return -1;
  for (int i = 0; i < WINSRV_MAX_WINDOWS; i++) {
    if (g_windows[i].in_use && g_windows[i].win == win)
      return i;
  }
  return -1;
}

static void enqueue_event_locked(winsrv_entry_t *e, const winsrv_event_t *ev) {
  if (e->event_count >= WINSRV_EVENT_QUEUE) {
    // Cola llena: descartar el más viejo.
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
  spin_init(&winsrv_lock);
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
#define WINSRV_USER_FLAGS                                                      \
  (WIN_FLAGS_NODECORATION | WIN_FLAGS_FIXED_SIZE | WIN_FLAGS_POPUP |           \
   WIN_FLAGS_RAW_INPUT)

int winsrv_create_window(task_t *owner, int x, int y, int w, int h,
                         const char *title) {
  return winsrv_create_window_ex(owner, x, y, w, h, title, 0);
}

int winsrv_create_window_ex(task_t *owner, int x, int y, int w, int h,
                            const char *title, uint32_t wflags) {
  if (!g_ready || !owner)
    return -1;
  wflags &= WINSRV_USER_FLAGS;
  // Un popup puede ser mas pequeno que una ventana normal (menus, tooltips).
  int min = (wflags & WIN_FLAGS_POPUP) ? 8 : 64;
  if (w < min || h < min || w > 2048 || h > 2048)
    return -1;

  char ktitle[64];
  long n = strncpy_from_user(ktitle, title, sizeof(ktitle));
  if (n < 0)
    return -1;

  // [Fase B] Reservar slot bajo lock. Marcamos in_use=1 aunque win
  // todavía sea NULL, para que otra CPU no coja el mismo slot.
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  int slot = find_free_slot_locked();
  if (slot < 0) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    LOG_ERR("[WINSRV] no hay slots libres");
    return -1;
  }
  g_windows[slot].in_use = 1;
  g_windows[slot].win = NULL;
  g_windows[slot].owner = owner;
  g_windows[slot].is_console = 0;
  g_windows[slot].event_head = 0;
  g_windows[slot].event_tail = 0;
  g_windows[slot].event_count = 0;
  spin_unlock_irqrestore(&winsrv_lock, flags);

  // Crear la ventana en el compositor. Fuera del lock (toca window_stack
  // y taskbar, con su propio lock).
  window_t *win = compositor_create_window(x, y, w, h, ktitle, wflags);
  if (!win) {
    // Liberar el slot reservado.
    flags = spin_lock_irqsave(&winsrv_lock);
    g_windows[slot].in_use = 0;
    g_windows[slot].owner = NULL;
    spin_unlock_irqrestore(&winsrv_lock, flags);
    LOG_ERR("[WINSRV] compositor_create_window falló");
    return -1;
  }

  // Publicar la ventana en el slot.
  flags = spin_lock_irqsave(&winsrv_lock);
  g_windows[slot].win = win;
  spin_unlock_irqrestore(&winsrv_lock, flags);

  // [AUTO-FOCUS] La primera ventana que se cree se lleva el foco
  // automáticamente. Sin esto, sin ratón no hay forma de teclear:
  // compositor_handle_key() descarta teclas si no hay ventana focused.
  //
  // Se hace AQUÍ y no en compositor_create_window porque las funciones
  // de winsrv_post_event() necesitan que el slot ya esté publicado
  // (buscan por find_slot_by_win_locked). Si lo hiciéramos antes, los
  // eventos FOCUS/BLUR se perderían.
  if (!(wflags & WIN_FLAGS_POPUP))
    compositor_focus_window(win);

  LOG_INFO("[WINSRV] ventana %d creada para task %u (%dx%d '%s')", slot,
           owner->id, w, h, ktitle);
  return slot;
}

int winsrv_destroy_window(task_t *owner, int win_id) {
  if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS)
    return -1;

  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }

  int was_console = (g_console_win_id == win_id);
  if (was_console) {
    g_console_win_id = -1;
  }

  window_t *win = e->win;
  e->in_use = 0;
  e->win = NULL;
  e->owner = NULL;
  e->is_console = 0;
  spin_unlock_irqrestore(&winsrv_lock, flags);

  // Fuera del lock: tty y compositor pueden coger otros locks.
  if (was_console) {
    tty_set_console_window(NULL);
  }
  if (win) {
    compositor_close_window(win);
  }

  LOG_INFO("[WINSRV] ventana %d destruida", win_id);
  return 0;
}

// ---------------------------------------------------------------------------
// Blit
// ---------------------------------------------------------------------------
// Todo el blit va bajo winsrv_lock. Copia solo el rectángulo [x, y, w, h]
// desde el buffer fuente (src_x, src_y, src_stride) al content_buffer
// de la ventana.
//
// El buffer fuente es el que userland tiene en su memoria. Normalmente
// es el buffer de píxeles completo de la ventana, y el usuario copia
// solo la región que ha modificado.
// ---------------------------------------------------------------------------
int winsrv_blit(task_t *owner, int win_id, int x, int y, int w, int h,
                int src_x, int src_y, int src_stride,
                const uint32_t *user_pixels) {
  if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS)
    return -1;
  if (!user_pixels || w <= 0 || h <= 0 || src_stride <= 0)
    return -1;
  if (src_x < 0 || src_y < 0)
    return -1;

  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner || !e->win) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }

  window_t *win = e->win;
  if (!win->content_buffer) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }

  // content_lock protege el intercambio de content_buffer en window_resize
  // (hilo del compositor, otra CPU). Orden: winsrv_lock -> content_lock.
  unsigned long cfl = spin_lock_irqsave(&win->content_lock);

  // Ajustar src_x/src_y si el rectángulo destino se sale de la ventana
  // por la izquierda/arriba.
  int local_src_x = src_x;
  int local_src_y = src_y;
  int dst_x = x, dst_y = y;
  int copy_w = w, copy_h = h;

  if (dst_x < 0) {
    local_src_x += -dst_x;
    copy_w -= -dst_x;
    dst_x = 0;
  }
  if (dst_y < 0) {
    local_src_y += -dst_y;
    copy_h -= -dst_y;
    dst_y = 0;
  }
  if (dst_x + copy_w > win->content_w)
    copy_w = win->content_w - dst_x;
  if (dst_y + copy_h > win->content_h)
    copy_h = win->content_h - dst_y;
  if (copy_w <= 0 || copy_h <= 0) {
    spin_unlock_irqrestore(&win->content_lock, cfl);
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return 0;
  }
  // Comprobar que el rectángulo fuente no se sale del buffer.
  if (local_src_x + copy_w > src_stride) {
    spin_unlock_irqrestore(&win->content_lock, cfl);
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }

  /*
   * Nunca acceder directamente al buffer de userland desde Ring 0.
   * access_ok() solo valida el rango de direcciones; la página puede estar
   * sin mapear o puede fallar durante un acceso. copy_from_user() usa la
   * tabla de exception-fixup y devuelve -EFAULT en vez de dejar que el
   * #PF llegue al camino fatal del kernel.
   *
   * Usamos un buffer de una sola fila para no reservar un bloque cuyo tamaño
   * dependa del src_stride. Cada fila se valida/copia mediante copy_from_user()
   * antes de tocar la fila correspondiente de la ventana.
   */
  /*
   * Mantener el buffer temporal pequeño es importante: winsrv_blit() puede
   * ejecutarse desde el contexto de un syscall con una pila de kernel
   * limitada. No reservamos un buffer proporcional al ancho de la ventana.
   * Copiamos cada fila en bloques pequeños mediante copy_from_user().
   */
  uint32_t row_buf[256];
  const int chunk_pixels = (int)(sizeof(row_buf) / sizeof(row_buf[0]));

  for (int row = 0; row < copy_h; row++) {
    int copied = 0;
    while (copied < copy_w) {
      int chunk = copy_w - copied;
      if (chunk > chunk_pixels)
        chunk = chunk_pixels;

      const uint32_t *src =
          user_pixels + (local_src_y + row) * src_stride + local_src_x + copied;
      size_t chunk_bytes = (size_t)chunk * sizeof(uint32_t);

      if (copy_from_user(row_buf, src, chunk_bytes) < 0) {
        spin_unlock_irqrestore(&win->content_lock, cfl);
    spin_unlock_irqrestore(&winsrv_lock, flags);
        return -EFAULT;
      }

      uint32_t *dst =
          win->content_buffer + (dst_y + row) * win->content_w + dst_x + copied;
      for (int col = 0; col < chunk; col++)
        dst[col] = row_buf[col];

      copied += chunk;
    }
  }
  // Solo esta zona de la superficie se recompone en el siguiente frame.
  window_mark_content_locked(win, (rect_t){dst_x, dst_y, copy_w, copy_h});

  int inv_x = win->x + dst_x;
  int inv_y = win->y + window_titlebar_h(win) + dst_y;
  int inv_w = copy_w;
  int inv_h = copy_h;

  spin_unlock_irqrestore(&win->content_lock, cfl);
    spin_unlock_irqrestore(&winsrv_lock, flags);

  compositor_invalidate_rect((rect_t){inv_x, inv_y, inv_w, inv_h});

  return 0;
}

// ---------------------------------------------------------------------------
// Poll event
// ---------------------------------------------------------------------------
static bool winsrv_has_events(void *arg) {
  winsrv_entry_t *e = (winsrv_entry_t *)arg;
  // Se llama con el lock de la wq cogido, NO con winsrv_lock.
  //
  // Leer event_count sin winsrv_lock es benigno:
  //   - Si es > 0, hay eventos: wait_event retorna 0 y el consumidor
  //     hará el pop (re-chequeando bajo winsrv_lock).
  //   - Si es 0, el wait_event vuelve a dormir. Si el productor encola
  //     justo después, llama a wake_up_all, que nos despertará y
  //     re-evaluará la condición.
  //   - Si es 0 pero otro consumidor se llevó el evento, la condición
  //     sigue siendo falsa y volvemos a dormir. Correcto.
  //
  // La única race real (leer 0 justo antes de que el productor encole)
  // se resuelve porque wait_event re-chequea la condición bajo el lock
  // de la wq tras insertarse, y el productor llama wake_up_all DESPUÉS
  // de encolar.
  return e->event_count > 0;
}

int winsrv_poll_event(task_t *owner, int win_id, winsrv_event_t *out_user_ev,
                      int blocking) {
  if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS || !out_user_ev)
    return -1;

  // Chequeo rápido bajo winsrv_lock: ¿la ventana es mía y tiene eventos?
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }
  int has_now = (e->event_count > 0);
  spin_unlock_irqrestore(&winsrv_lock, flags);

  if (!has_now) {
    if (!blocking)
      return 0;
    // wait_event_interruptible usa el lock de la wq, no winsrv_lock.
    int rc = wait_event_interruptible(&e->event_wq, winsrv_has_events, e);
    if (rc < 0)
      return -1;
  }

  // Pop con winsrv_lock. Re-validamos la ventana: pudo cerrarse
  // mientras dormíamos, o pudo consumirse el evento.
  flags = spin_lock_irqsave(&winsrv_lock);
  if (!e->in_use || e->owner != owner) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }
  if (e->event_count == 0) {
    // Otra CPU se llevó el evento.
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return 0;
  }
  winsrv_event_t ev = e->events[e->event_head];
  e->event_head = (e->event_head + 1) % WINSRV_EVENT_QUEUE;
  e->event_count--;
  spin_unlock_irqrestore(&winsrv_lock, flags);

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

  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }

  if (g_console_win_id >= 0 && g_console_win_id != win_id) {
    g_windows[g_console_win_id].is_console = 0;
  }
  g_console_win_id = win_id;
  e->is_console = 1;

  window_t *win = e->win;
  spin_unlock_irqrestore(&winsrv_lock, flags);

  // tty_set_console_window fuera del lock.
  tty_set_console_window(win);

  LOG_INFO("[WINSRV] ventana %d registrada como consola", win_id);
  return 0;
}

int winsrv_set_raw_input(task_t *owner, int win_id, int enable) {
  if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS)
    return -1;
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner || !e->win) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }
  if (enable)
    e->win->flags |= WIN_FLAGS_RAW_INPUT;
  else
    e->win->flags &= ~WIN_FLAGS_RAW_INPUT;
  spin_unlock_irqrestore(&winsrv_lock, flags);
  return 0;
}

// ---------------------------------------------------------------------------
// API interna para el compositor y el VFS
// ---------------------------------------------------------------------------
void winsrv_post_event(window_t *win, uint32_t type, int32_t x, int32_t y,
                       uint32_t data) {
  if (!g_ready || !win)
    return;

  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  int slot = find_slot_by_win_locked(win);
  if (slot < 0) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return;
  }

  winsrv_entry_t *e = &g_windows[slot];
  winsrv_event_t ev = {
      .type = type,
      .x = x,
      .y = y,
      .data = data,
  };
  enqueue_event_locked(e, &ev);
  spin_unlock_irqrestore(&winsrv_lock, flags);

  // wake_up_all FUERA de winsrv_lock: coge el lock de la wq.
  wake_up_all(&e->event_wq);
}

void winsrv_console_output(char c) {
  if (!g_ready)
    return;

  // Resolver la ventana de consola bajo el lock.
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  int cid = g_console_win_id;
  if (cid < 0 || cid >= WINSRV_MAX_WINDOWS || !g_windows[cid].in_use ||
      !g_windows[cid].win) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return;
  }
  window_t *win = g_windows[cid].win;
  spin_unlock_irqrestore(&winsrv_lock, flags);

  winsrv_post_event(win, WINSRV_EV_OUTPUT, (int32_t)(uint8_t)c, 0, 0);
}

int winsrv_has_window(window_t *win) {
  if (!g_ready)
    return 0;
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  int slot = find_slot_by_win_locked(win);
  spin_unlock_irqrestore(&winsrv_lock, flags);
  return slot >= 0;
}

void winsrv_cleanup_task(task_t *owner) {
  if (!g_ready || !owner)
    return;

  // Recopilar las ventanas a cerrar bajo el lock. El cierre se hace
  // fuera para no coger el lock del compositor desde dentro del nuestro.
  window_t *to_close[WINSRV_MAX_WINDOWS];
  int n_close = 0;
  int reset_console = 0;
  int closed_slots[WINSRV_MAX_WINDOWS];
  int n_slots = 0;

  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  for (int i = 0; i < WINSRV_MAX_WINDOWS; i++) {
    winsrv_entry_t *e = &g_windows[i];
    if (e->in_use && e->owner == owner) {
      window_t *win = e->win;
      if (g_console_win_id == i) {
        g_console_win_id = -1;
        reset_console = 1;
      }
      e->in_use = 0;
      e->win = NULL;
      e->owner = NULL;
      e->is_console = 0;
      if (win) {
        to_close[n_close++] = win;
      }
      closed_slots[n_slots++] = i;
    }
  }
  spin_unlock_irqrestore(&winsrv_lock, flags);

  if (reset_console)
    tty_set_console_window(NULL);

  for (int i = 0; i < n_close; i++) {
    compositor_close_window(to_close[i]);
  }
  for (int i = 0; i < n_slots; i++) {
    LOG_INFO("[WINSRV] limpieza: ventana %d de task %u muerta", closed_slots[i],
             owner->id);
  }
}

int winsrv_set_icon(task_t *owner, int win_id, const char *path) {
  if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS || !path)
    return -1;
  if (!path[0])
    return -1;

  // [PIVOT] `path` viene resuelto y validado por sys_win_set_icon_k
  // (resolve_user_path). Es un puntero de KERNEL, con '/' inicial, tipo
  // "/initrd/system/icons/terminal-window-dark.bmp". Lo resolvemos por
  // VFS: el mount de tarfs en /initrd lo encuentra.
  //
  // [NOTA] win_set_icon_bmp() guarda el puntero `icon_bmp_node` en la
  // ventana. Aunque has_icon_cache=1 hace que no se vuelva a leer (ver
  // draw_app_icon), dejarlo apuntando a memoria liberada es UB.
  // Allocamos node + data con kmalloc y los dejamos vivos. Es un leak
  // bounded (máx WINSRV_MAX_WINDOWS iconos), aceptable mientras la API
  // no cambie a "decode-and-copy".
  void *buf = NULL;
  size_t size = 0;
  int rc = vfs_read_all(path, &buf, &size);
  if (rc != 0) {
    LOG_ERR("[WINSRV] set_icon: vfs_read_all('%s') falló: %d", path, rc);
    return -ENOENT;
  }

  tar_node_t *node = (tar_node_t *)kzalloc(sizeof(tar_node_t));
  if (!node) {
    kfree(buf);
    return -ENOMEM;
  }
  node->data = (uint8_t *)buf;
  node->size = size;
  node->is_dir = 0;
  {
    size_t plen = strlen(path);
    if (plen >= sizeof(node->name))
      plen = sizeof(node->name) - 1;
    for (size_t i = 0; i < plen; i++)
      node->name[i] = path[i];
    node->name[plen] = '\0';
  }

  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner || !e->win) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    kfree(node);
    kfree(buf);
    return -1;
  }
  window_t *win = e->win;
  spin_unlock_irqrestore(&winsrv_lock, flags);

  LOG_DEBUG("[WINSRV] set_icon: '%s' (%llu bytes)", path,
            (unsigned long long)size);

  // El hilo del compositor es el único que toca la ventana y su item de la
  // taskbar/Dock: se le encola el nodo. Si el BMP no es decodificable, el
  // Dock cae a una baldosa con el símbolo (no hay error síncrono). node y buf
  // permanecen vivos (leak acotado por WINSRV_MAX_WINDOWS, ver nota arriba).
  compositor_set_icon_bmp(win, node);
  return 0;
}

int winsrv_set_title(task_t *owner, int win_id, const char *title) {
  if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS || !title)
    return -1;
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner || !e->win) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }
  window_t *win = e->win;
  spin_unlock_irqrestore(&winsrv_lock, flags);
  compositor_set_title(win, title);
  return 0;
}

int winsrv_set_state(task_t *owner, int win_id, int state) {
  if (!g_ready || win_id < 0 || win_id >= WINSRV_MAX_WINDOWS)
    return -1;
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  winsrv_entry_t *e = &g_windows[win_id];
  if (!e->in_use || e->owner != owner || !e->win) {
    spin_unlock_irqrestore(&winsrv_lock, flags);
    return -1;
  }
  window_t *win = e->win;
  spin_unlock_irqrestore(&winsrv_lock, flags);
  switch (state) {
  case WINSRV_STATE_MINIMIZE:
    compositor_minimize_window(win);
    break;
  case WINSRV_STATE_MAXIMIZE:
    compositor_set_maximized(win, 1);
    break;
  case WINSRV_STATE_RESTORE:
    compositor_set_maximized(win, 0);
    break;
  default:
    return -1;
  }
  return 0;
}

void winsrv_post_to_focused(uint32_t type, int32_t x, int32_t y,
                            uint32_t data) {
  if (!g_ready)
    return;

  window_t *found = NULL;
  unsigned long flags = spin_lock_irqsave(&winsrv_lock);
  for (int i = 0; i < WINSRV_MAX_WINDOWS; i++) {
    if (!g_windows[i].in_use || !g_windows[i].win)
      continue;
    window_t *w = g_windows[i].win;
    if ((w->flags & WIN_FLAGS_FOCUSED) && !(w->flags & WIN_FLAGS_HIDDEN)) {
      found = w;
      break;
    }
  }
  spin_unlock_irqrestore(&winsrv_lock, flags);

  if (found)
    winsrv_post_event(found, type, x, y, data);
}