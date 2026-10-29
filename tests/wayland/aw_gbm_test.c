// tests/wayland/aw_gbm_test.c
//
// L7 hito 3: cliente EGL/GBM de diagnostico para Mesa sobre el DRM de Linux.
//
//   1. abre el nodo render (renderD128, o argv[1]) y crea un gbm_device;
//   2. EGL (plataforma GBM) + contexto GLES2 sin superficie;
//   3. un gbm_bo XRGB8888 LINEAR con USE_RENDERING (el que Mesa asigna con el
//      driver de GPU: crocus en Haswell; llvmpipe/kms_swrast en QEMU) se
//      importa como EGLImage, se pinta con GL (barras con scissor + un triangulo
//      con shaders) y se lee de vuelta para comprobar el resultado;
//   4. el bo se exporta como dma-buf (gbm_bo_get_fd) y se entrega a
//      aurora-wayland con zwp_linux_dmabuf_v1 (modificador LINEAR) en un
//      toplevel. `--no-wayland` salta este paso.
//
// Cada paso imprime "[aw_gbm_test] ..." para el log serie; el resultado final es
// "[aw_gbm_test] ALL PASS" o la primera linea "FAIL". Pensado para la Portegé
// con `MESA_LOADER_DRIVER_OVERRIDE=crocus` (y `MESA_DEBUG=1 INTEL_DEBUG=...`).
// Las declaraciones de GBM/EGL/GLES2 son locales: no hacen falta los -dev.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define W 320
#define H 200
#define FOURCC_XRGB8888 0x34325258u /* DRM_FORMAT_XRGB8888 */

/* ---- GBM */
struct gbm_device;
struct gbm_bo;
#define GBM_BO_USE_RENDERING (1u << 2)
#define GBM_BO_USE_LINEAR (1u << 4)
struct gbm_device *gbm_create_device(int fd);
void gbm_device_destroy(struct gbm_device *);
const char *gbm_device_get_backend_name(struct gbm_device *);
struct gbm_bo *gbm_bo_create(struct gbm_device *, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags);
uint32_t gbm_bo_get_stride(struct gbm_bo *);
int gbm_bo_get_fd(struct gbm_bo *);
uint64_t gbm_bo_get_modifier(struct gbm_bo *);
void gbm_bo_destroy(struct gbm_bo *);

/* ---- EGL */
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface, *EGLImage;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum;
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_SURFACE_TYPE 0x3033
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_PLATFORM_GBM_KHR 0x31D7
#define EGL_NATIVE_PIXMAP_KHR 0x30B0
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
EGLDisplay eglGetPlatformDisplay(EGLenum, void *, const intptr_t *);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
const char *eglQueryString(EGLDisplay, EGLint);
EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLImage eglCreateImage(EGLDisplay, EGLContext, EGLenum, void *, const intptr_t *);
EGLBoolean eglDestroyImage(EGLDisplay, EGLImage);
void *(*eglGetProcAddress(const char *))(void);
EGLint eglGetError(void);

/* ---- GLES2 */
typedef unsigned int GLuint, GLenum;
typedef int GLint, GLsizei;
typedef float GLfloat;
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SCISSOR_TEST 0x0C11
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
void glGenRenderbuffers(GLsizei, GLuint *);
void glBindRenderbuffer(GLenum, GLuint);
void glGenFramebuffers(GLsizei, GLuint *);
void glBindFramebuffer(GLenum, GLuint);
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
GLenum glCheckFramebufferStatus(GLenum);
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glEnable(GLenum);
void glDisable(GLenum);
void glScissor(GLint, GLint, GLsizei, GLsizei);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClear(GLuint);
void glFinish(void);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
const unsigned char *glGetString(GLenum);
GLenum glGetError(void);
GLuint glCreateShader(GLenum);
void glShaderSource(GLuint, GLsizei, const char *const *, const GLint *);
void glCompileShader(GLuint);
void glGetShaderiv(GLuint, GLenum, GLint *);
GLuint glCreateProgram(void);
void glAttachShader(GLuint, GLuint);
void glLinkProgram(GLuint);
void glGetProgramiv(GLuint, GLenum, GLint *);
void glUseProgram(GLuint);
void glBindAttribLocation(GLuint, GLuint, const char *);
void glEnableVertexAttribArray(GLuint);
void glVertexAttribPointer(GLuint, GLint, GLenum, unsigned char, GLsizei, const void *);
void glDrawArrays(GLenum, GLint, GLsizei);

static int fail(const char *what) {
  printf("[aw_gbm_test] FAIL %s (errno=%d egl=0x%x gl=0x%x)\n", what, errno, eglGetError(), glGetError());
  fflush(stdout);
  return 1;
}

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  GLint ok = 0;
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  return ok ? s : 0;
}

/* Pinta en el framebuffer actual (W x H): barras con scissor arriba (para
 * comprobar el color) y un triangulo con degradado debajo (shaders). */
static int draw_scene(void) {
  static const GLfloat bars[6][3] = {{1, .23f, .19f}, {1, .58f, 0}, {1, .8f, 0},
                                     {.2f, .78f, .35f}, {.04f, .52f, 1}, {.75f, .35f, .95f}};
  glViewport(0, 0, W, H);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, W, H);
  glClearColor(.1f, .1f, .12f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  for (int i = 0; i < 6; i++) {
    glScissor(i * W / 6, H - 40, W / 6 + 1, 40);
    glClearColor(bars[i][0], bars[i][1], bars[i][2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glDisable(GL_SCISSOR_TEST);
  GLuint vs = compile(GL_VERTEX_SHADER,
                      "attribute vec2 p; attribute vec3 c; varying vec3 vc;"
                      "void main(){ vc = c; gl_Position = vec4(p, 0.0, 1.0); }");
  GLuint fs = compile(GL_FRAGMENT_SHADER,
                      "precision mediump float; varying vec3 vc;"
                      "void main(){ gl_FragColor = vec4(vc, 1.0); }");
  if (!vs || !fs)
    return fail("compilar shaders");
  GLuint prog = glCreateProgram();
  GLint ok = 0;
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glBindAttribLocation(prog, 0, "p");
  glBindAttribLocation(prog, 1, "c");
  glLinkProgram(prog);
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok)
    return fail("enlazar programa");
  glUseProgram(prog);
  static const GLfloat tri[] = {-.8f, -.9f, 1, 0, 0, .8f, -.9f, 0, 1, 0, 0, .6f, 0, 0, 1};
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(GLfloat), tri);
  glVertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(GLfloat), tri + 2);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glViewport(0, 0, W, H - 40);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glFinish();
  return 0;
}

/* ---- Wayland */
static struct wl_compositor *g_comp;
static struct xdg_wm_base *g_wm;
static struct zwp_linux_dmabuf_v1 *g_dmabuf;
static int g_configured, g_xrgb, g_linear;
static struct wl_display *g_dpy;

static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *o, uint32_t f) {
  (void)d; (void)o;
  if (f == FOURCC_XRGB8888) g_xrgb = 1;
}
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *o, uint32_t f, uint32_t hi, uint32_t lo) {
  if (f == FOURCC_XRGB8888 && hi == 0 && lo == 0) g_linear = 1;
  dmabuf_format(d, o, f);
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_l = {dmabuf_format, dmabuf_modifier};
static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, "wl_compositor"))
    g_comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, "xdg_wm_base"))
    g_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(iface, "zwp_linux_dmabuf_v1")) {
    g_dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3);
    zwp_linux_dmabuf_v1_add_listener(g_dmabuf, &dmabuf_l, NULL);
  }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};
static void xs_conf(void *d, struct xdg_surface *xs, uint32_t s) { (void)d; xdg_surface_ack_configure(xs, s); g_configured = 1; }
static const struct xdg_surface_listener xs_l = {xs_conf};
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; exit(0); }
static const struct xdg_toplevel_listener tl_l = {tl_conf, tl_close, NULL, NULL};

static int present(int bo_fd, uint32_t stride) {
  struct wl_display *dpy = g_dpy = wl_display_connect(NULL);
  if (!dpy)
    return fail("conectar a Wayland");
  struct wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &reg_l, NULL);
  wl_display_roundtrip(dpy);
  wl_display_roundtrip(dpy);
  printf("[aw_gbm_test] wayland: linux_dmabuf=%d xrgb=%d linear=%d\n", g_dmabuf != NULL, g_xrgb, g_linear);
  if (!g_comp || !g_wm || !g_dmabuf || !g_xrgb || !g_linear)
    return fail("globales de Wayland");
  xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
  struct wl_surface *surf = wl_compositor_create_surface(g_comp);
  struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, surf);
  xdg_surface_add_listener(xs, &xs_l, NULL);
  struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(tl, &tl_l, NULL);
  xdg_toplevel_set_title(tl, "AW GBM Test");
  xdg_toplevel_set_app_id(tl, "aurora.gbmtest");
  wl_surface_commit(surf);
  wl_display_roundtrip(dpy);
  if (!g_configured)
    return fail("sin configure");
  struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(g_dmabuf);
  zwp_linux_buffer_params_v1_add(params, bo_fd, 0, 0, stride, 0, 0);
  struct wl_buffer *buf = zwp_linux_buffer_params_v1_create_immed(params, W, H, FOURCC_XRGB8888, 0);
  zwp_linux_buffer_params_v1_destroy(params);
  wl_surface_attach(surf, buf, 0, 0);
  wl_surface_damage(surf, 0, 0, W, H);
  wl_surface_commit(surf);
  if (wl_display_roundtrip(dpy) < 0)
    return fail("el servidor rechazo el buffer");
  printf("[aw_gbm_test] presentado por linux-dmabuf\n");
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv) {
  const char *node = "/dev/dri/renderD128";
  int want_wl = 1, hold = 1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--no-wayland")) want_wl = 0;
    else if (!strcmp(argv[i], "--exit")) hold = 0;
    else node = argv[i];
  }
  setvbuf(stdout, NULL, _IONBF, 0);
  int fd = open(node, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return fail("abrir el nodo render");
  struct gbm_device *gbm = gbm_create_device(fd);
  if (!gbm)
    return fail("gbm_create_device");
  printf("[aw_gbm_test] gbm backend=%s\n", gbm_device_get_backend_name(gbm));
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
  EGLint maj, min;
  if (!dpy || !eglInitialize(dpy, &maj, &min))
    return fail("eglInitialize");
  printf("[aw_gbm_test] EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));
  const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
  printf("[aw_gbm_test] EGL_EXT image_dma_buf_import=%d surfaceless=%d\n",
         ext && strstr(ext, "EGL_EXT_image_dma_buf_import") != NULL,
         ext && strstr(ext, "EGL_KHR_surfaceless_context") != NULL);
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg;
  EGLint n = 0, ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
  if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) {
    EGLint cb[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    if (!eglChooseConfig(dpy, cb, &cfg, 1, &n) || n < 1)
      return fail("eglChooseConfig");
  }
  EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
  if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
    return fail("contexto GLES2 sin superficie");
  printf("[aw_gbm_test] GL_VENDOR=%s\n[aw_gbm_test] GL_RENDERER=%s\n[aw_gbm_test] GL_VERSION=%s\n",
         glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));

  struct gbm_bo *bo = gbm_bo_create(gbm, W, H, FOURCC_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
  if (!bo)
    return fail("gbm_bo_create (XRGB8888 LINEAR RENDERING)");
  uint32_t stride = gbm_bo_get_stride(bo);
  printf("[aw_gbm_test] bo %dx%d stride=%u modifier=0x%llx\n", W, H, stride, (unsigned long long)gbm_bo_get_modifier(bo));
  EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
  if (!img)
    return fail("eglCreateImage(gbm_bo)");
  void (*img_to_rb)(GLenum, EGLImage) = (void (*)(GLenum, EGLImage))eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
  if (!img_to_rb)
    return fail("glEGLImageTargetRenderbufferStorageOES");
  GLuint rb, fb;
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  img_to_rb(GL_RENDERBUFFER, img);
  glGenFramebuffers(1, &fb);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    return fail("framebuffer incompleto");
  if (draw_scene())
    return 1;
  /* La 4.ª barra (verde, .2 .78 .35) ocupa las 40 filas altas de GL (y >= H-40). */
  unsigned char px[4] = {0};
  glReadPixels(3 * W / 6 + 10, H - 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  printf("[aw_gbm_test] pixel de la barra verde = %02x%02x%02x (esperado ~33c759)\n", px[0], px[1], px[2]);
  if (abs(px[0] - 0x33) > 3 || abs(px[1] - 0xc7) > 3 || abs(px[2] - 0x59) > 3)
    return fail("el pixel leido no es el pintado");
  printf("[aw_gbm_test] render GL OK\n");

  if (want_wl) {
    int bo_fd = gbm_bo_get_fd(bo);
    if (bo_fd < 0)
      return fail("gbm_bo_get_fd (PRIME)");
    if (present(bo_fd, stride))
      return 1;
  }
  printf("[aw_gbm_test] ALL PASS\n");
  if (!want_wl || !hold)
    return 0;
  for (;;) {
    wl_display_flush(g_dpy);
    struct pollfd pfd = {.fd = wl_display_get_fd(g_dpy), .events = POLLIN};
    if (poll(&pfd, 1, 1000) > 0 && wl_display_dispatch(g_dpy) < 0)
      break;
  }
  return 0;
}
