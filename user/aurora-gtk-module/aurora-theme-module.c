// Modulo GTK3 de Aurora (GTK_MODULES=aurora-theme, GTK_PATH=/usr/lib/aurora-gtk).
//
// Sondea el kernel (ASYS_THEME_GET) y cambia el tema GTK en vivo entre
// "Aurora" y "Aurora-light" cuando el usuario alterna el modo claro/oscuro.
#include <gtk/gtk.h>
#include <sys/syscall.h>
#include <unistd.h>

static int g_last = -1;

static gboolean tick(gpointer data) {
  (void)data;
  long v = syscall(0x1070); /* ASYS_BASE 0x1000 + 0x70 */
  if (v < 0)
    return TRUE;
  int light = (int)(v & 1);
  if (light == g_last)
    return TRUE;
  g_last = light;
  /* GTK_THEME (lo pone el lanzador) manda sobre la propiedad: se quita. */
  g_unsetenv("GTK_THEME");
  GtkSettings *s = gtk_settings_get_default();
  if (s)
    g_object_set(s, "gtk-theme-name", light ? "Aurora-light" : "Aurora", NULL);
  return TRUE;
}

G_MODULE_EXPORT void gtk_module_init(gint *argc, gchar ***argv) {
  (void)argc;
  (void)argv;
  g_timeout_add(400, tick, NULL);
}
