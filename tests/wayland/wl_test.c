// tests/wayland/wl_test.c
//
// Hito de la Fase 1 del plan Wayland: libwayland-server y libwayland-client
// de la distro (sin tocar) hablan entre si en AuroraOS. Un proceso hace de
// compositor minimo (wl_compositor + wl_shm), el otro de cliente: crea un
// pool wl_shm (memfd + SCM_RIGHTS), un wl_buffer, lo adjunta a un wl_surface y
// hace commit. El servidor lee el buffer por mmap compartido y comprueba el
// patron de pixeles.
//
// Compilar con tests/wayland/build.sh (contra las cabeceras de libwayland-dev).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

#define W 64
#define H 32
#define STRIDE (W * 4)
#define SIZE (STRIDE * H)
#define SOCK "wl-test"

static int server_result = -1;
static struct wl_display *g_display;

// Terminar desde un idle: el dispatch en curso termina de atender la
// peticion wl_display.sync del cliente y el release se envia antes de parar.
static void finish_idle(void *data) {
  (void)data;
  wl_display_flush_clients(g_display);
  wl_display_terminate(g_display);
}
static void finish_soon(void) {
  wl_event_loop_add_idle(wl_display_get_event_loop(g_display), finish_idle,
                         NULL);
}

// ---------------------------------------------------------------------------
// Servidor
// ---------------------------------------------------------------------------
struct surface {
  struct wl_resource *buffer;
};

static void surf_destroy(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  wl_resource_destroy(r);
}
static void surf_attach(struct wl_client *c, struct wl_resource *r,
                        struct wl_resource *buf, int32_t x, int32_t y) {
  (void)c;
  (void)x;
  (void)y;
  ((struct surface *)wl_resource_get_user_data(r))->buffer = buf;
}
static void surf_noop_damage(struct wl_client *c, struct wl_resource *r,
                             int32_t x, int32_t y, int32_t w, int32_t h) {
  (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb) {
  (void)c; (void)r; (void)cb;
}
static void surf_noop_region(struct wl_client *c, struct wl_resource *r,
                             struct wl_resource *reg) {
  (void)c; (void)r; (void)reg;
}
static void surf_noop_i32(struct wl_client *c, struct wl_resource *r, int32_t v) {
  (void)c; (void)r; (void)v;
}
static void surf_offset(struct wl_client *c, struct wl_resource *r, int32_t x,
                        int32_t y) {
  (void)c; (void)r; (void)x; (void)y;
}

static void surf_commit(struct wl_client *c, struct wl_resource *r) {
  (void)c;
  struct surface *s = wl_resource_get_user_data(r);
  if (!s->buffer) {
    printf("[wl_test] FAIL commit sin buffer\n");
    server_result = 1;
    finish_soon();
    return;
  }
  struct wl_shm_buffer *sb = wl_shm_buffer_get(s->buffer);
  if (!sb) {
    printf("[wl_test] FAIL wl_shm_buffer_get\n");
    server_result = 1;
    finish_soon();
    return;
  }
  wl_shm_buffer_begin_access(sb);
  uint32_t *px = wl_shm_buffer_get_data(sb);
  int w = wl_shm_buffer_get_width(sb), h = wl_shm_buffer_get_height(sb);
  int ok = (w == W && h == H);
  for (int i = 0; ok && i < W * H; i++)
    if (px[i] != (0xFF000000u | (uint32_t)i))
      ok = 0;
  wl_shm_buffer_end_access(sb);
  printf("[wl_test] %s servidor lee el buffer del cliente (%dx%d)\n",
         ok ? "PASS" : "FAIL", w, h);
  server_result = ok ? 0 : 1;
  wl_buffer_send_release(s->buffer);
  finish_soon();
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surf_destroy,
    .attach = surf_attach,
    .damage = surf_noop_damage,
    .frame = surf_frame,
    .set_opaque_region = surf_noop_region,
    .set_input_region = surf_noop_region,
    .commit = surf_commit,
    .set_buffer_transform = surf_noop_i32,
    .set_buffer_scale = surf_noop_i32,
    .damage_buffer = surf_noop_damage,
    .offset = surf_offset,
};

static void surface_resource_destroy(struct wl_resource *r) {
  free(wl_resource_get_user_data(r));
}

static void comp_create_surface(struct wl_client *client,
                                struct wl_resource *res, uint32_t id) {
  struct surface *s = calloc(1, sizeof(*s));
  struct wl_resource *r = wl_resource_create(
      client, &wl_surface_interface, wl_resource_get_version(res), id);
  wl_resource_set_implementation(r, &surface_impl, s, surface_resource_destroy);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r,
                               uint32_t id) {
  (void)c; (void)r; (void)id;
}
static const struct wl_compositor_interface comp_impl = {
    .create_surface = comp_create_surface,
    .create_region = comp_create_region,
};
static void bind_compositor(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id) {
  (void)data;
  struct wl_resource *r =
      wl_resource_create(client, &wl_compositor_interface, version, id);
  wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

// ---------------------------------------------------------------------------
// Cliente
// ---------------------------------------------------------------------------
static struct wl_compositor *c_comp;
static struct wl_shm *c_shm;
static int released;

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, "wl_compositor"))
    c_comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "wl_shm"))
    c_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  (void)ver;
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {
  (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};
static void buf_release(void *d, struct wl_buffer *b) {
  (void)d; (void)b;
  released = 1;
}
static const struct wl_buffer_listener buf_listener = {buf_release};

static int run_client(void) {
  struct wl_display *d = wl_display_connect(SOCK);
  if (!d) {
    printf("[wl_test] FAIL cliente: wl_display_connect (errno=%d)\n", errno);
    return 10;
  }
  printf("[wl_test] PASS wl_display_connect\n");
  struct wl_registry *reg = wl_display_get_registry(d);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0) {
    printf("[wl_test] FAIL roundtrip del registry\n");
    return 11;
  }
  if (!c_comp || !c_shm) {
    printf("[wl_test] FAIL globales wl_compositor/wl_shm (%p %p)\n",
           (void *)c_comp, (void *)c_shm);
    return 12;
  }
  printf("[wl_test] PASS registry: wl_compositor + wl_shm\n");

  int fd = memfd_create("wl-shm", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, SIZE) != 0) {
    printf("[wl_test] FAIL memfd/ftruncate (errno=%d)\n", errno);
    return 13;
  }
  uint32_t *px = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) {
    printf("[wl_test] FAIL mmap shared (errno=%d)\n", errno);
    return 14;
  }
  for (int i = 0; i < W * H; i++)
    px[i] = 0xFF000000u | (uint32_t)i;

  struct wl_shm_pool *pool = wl_shm_create_pool(c_shm, fd, SIZE);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      pool, 0, W, H, STRIDE, WL_SHM_FORMAT_ARGB8888);
  wl_buffer_add_listener(buf, &buf_listener, NULL);
  wl_shm_pool_destroy(pool);
  close(fd);

  struct wl_surface *surf = wl_compositor_create_surface(c_comp);
  wl_surface_attach(surf, buf, 0, 0);
  wl_surface_damage(surf, 0, 0, W, H);
  wl_surface_commit(surf);
  if (wl_display_roundtrip(d) < 0) {
    printf("[wl_test] FAIL roundtrip tras commit\n");
    return 15;
  }
  for (int i = 0; i < 10 && !released; i++)
    if (wl_display_dispatch(d) < 0)
      break;
  printf("[wl_test] %s wl_buffer.release recibido\n", released ? "PASS" : "FAIL");
  wl_display_disconnect(d);
  return released ? 0 : 16;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (!getenv("XDG_RUNTIME_DIR"))
    setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);

  g_display = wl_display_create();
  if (!g_display) {
    printf("[wl_test] FAIL wl_display_create\n");
    return 1;
  }
  if (wl_display_add_socket(g_display, SOCK) != 0) {
    printf("[wl_test] FAIL wl_display_add_socket (errno=%d)\n", errno);
    return 2;
  }
  printf("[wl_test] PASS wl_display_add_socket (%s)\n", SOCK);
  wl_display_init_shm(g_display);
  wl_global_create(g_display, &wl_compositor_interface, 4, NULL,
                   bind_compositor);

  pid_t pid = fork();
  if (pid == 0)
    _exit(run_client());

  wl_display_run(g_display);
  int st = 0;
  waitpid(pid, &st, 0);
  int crc = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
  wl_display_destroy(g_display);
  int ok = (server_result == 0 && crc == 0);
  printf("[wl_test] %s (servidor=%d cliente=%d)\n", ok ? "ALL PASS" : "FAILED",
         server_result, crc);
  return ok ? 0 : 1;
}
