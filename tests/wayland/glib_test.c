// tests/wayland/glib_test.c
// Comprueba el comportamiento del bucle principal de GLib (que usan GTK y Qt)
// sobre la capa Linux de Aurora: plazos de timeout, numero de iteraciones
// (un bucle que gira sin dormir es el sintoma de poll/eventfd mal), hilos que
// despiertan el contexto y GTask. Imprime una linea por prueba y GLIB-OK/FAIL.
#include <glib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static GMainLoop *loop;
static int ticks, iterations, done_tasks;
static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("[glib_test] FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("[glib_test] ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static gboolean on_tick(gpointer d) { (void)d; ticks++; return G_SOURCE_CONTINUE; }
static gboolean on_stop(gpointer d) { (void)d; g_main_loop_quit(loop); return G_SOURCE_REMOVE; }

// Fuente que solo cuenta cuantas veces se consulta el bucle.
static gboolean cnt_prepare(GSource *s, gint *t) { (void)s; iterations++; *t = -1; return FALSE; }
static gboolean cnt_check(GSource *s) { (void)s; return FALSE; }
static gboolean cnt_dispatch(GSource *s, GSourceFunc cb, gpointer d) { (void)s; (void)cb; (void)d; return TRUE; }
static GSourceFuncs cnt_funcs = {cnt_prepare, cnt_check, cnt_dispatch, NULL, NULL, NULL};

static gboolean task_done(gpointer d) { (void)d; if (++done_tasks == 200) g_main_loop_quit(loop); return G_SOURCE_REMOVE; }
static void worker(gpointer data, gpointer user) { (void)data; (void)user; g_usleep(200); g_idle_add(task_done, NULL); }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(void) {
  // 1. Timeout: 40 ticks de 50 ms = ~2 s.
  loop = g_main_loop_new(NULL, FALSE);
  GSource *c = g_source_new(&cnt_funcs, sizeof(GSource));
  g_source_attach(c, NULL);
  g_timeout_add(50, on_tick, NULL);
  g_timeout_add(2000, on_stop, NULL);
  double t0 = now();
  g_main_loop_run(loop);
  double dt = now() - t0;
  CHECK(dt > 1.8 && dt < 2.6, "timeout de 2 s duro %.2f s", dt);
  CHECK(ticks >= 30 && ticks <= 45, "ticks de 50 ms: %d", ticks);
  CHECK(iterations < 400, "iteraciones del bucle en reposo: %d (sin giro)", iterations);
  g_source_destroy(c);

  // 2. Hilos que completan trabajo con g_idle_add (despiertan el contexto).
  GThreadPool *pool = g_thread_pool_new(worker, NULL, 4, FALSE, NULL);
  for (int i = 0; i < 200; i++)
    g_thread_pool_push(pool, GINT_TO_POINTER(i + 1), NULL);
  g_timeout_add(8000, on_stop, NULL);
  t0 = now();
  g_main_loop_run(loop);
  CHECK(done_tasks == 200, "200 tareas de hilos completadas (%d) en %.2f s", done_tasks, now() - t0);
  g_thread_pool_free(pool, TRUE, TRUE);

  // 3. Inactividad: tras todo, el bucle debe dormir (iteraciones acotadas).
  iterations = 0;
  c = g_source_new(&cnt_funcs, sizeof(GSource));
  g_source_attach(c, NULL);
  g_timeout_add(1000, on_stop, NULL);
  g_main_loop_run(loop);
  CHECK(iterations < 100, "1 s de reposo: %d iteraciones", iterations);
  printf(fails ? "GLIB-FAIL\n" : "GLIB-OK\n");
  return fails != 0;
}
