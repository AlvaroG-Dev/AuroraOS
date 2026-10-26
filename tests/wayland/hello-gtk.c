// Minima app GTK3: una ventana con una etiqueta y un boton.
#include <gtk/gtk.h>
static void on_click(GtkWidget *b, gpointer d) {
  (void)d;
  gtk_button_set_label(GTK_BUTTON(b), "Pulsado");
  g_print("[hello-gtk] clic\n");
}
int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(w), "Hola GTK en Aurora");
  gtk_window_set_default_size(GTK_WINDOW(w), 360, 160);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 16);
  GtkWidget *l = gtk_label_new("GTK3 sin tocar, dentro de Aurora");
  GtkWidget *b = gtk_button_new_with_label("Pulsame");
  g_signal_connect(b, "clicked", G_CALLBACK(on_click), NULL);
  gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(w), box);
  g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  gtk_widget_show_all(w);
  g_print("[hello-gtk] mapped\n");
  gtk_main();
  return 0;
}
