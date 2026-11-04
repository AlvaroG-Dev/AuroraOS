#define _GNU_SOURCE
#include "gpuimp.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface, *EGLImage;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum, GLuint, GLenum;
typedef int GLint, GLsizei;

#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_PLATFORM_GBM_KHR 0x31D7
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA 0x1908
#define GL_BGRA_EXT 0x80E1
#define GL_UNSIGNED_BYTE 0x1401

static struct {
  int tried, ok;
  void *egl, *gles, *gbm;
  EGLDisplay dpy;
  GLuint rb, fb;
  uint8_t *buf;
  size_t buf_len;
  int logged_ok, fails;
  void *(*gbm_create_device)(int);
  EGLDisplay (*eglGetPlatformDisplay)(EGLenum, void *, const intptr_t *);
  EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
  EGLBoolean (*eglBindAPI)(EGLenum);
  EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
  EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
  EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  EGLImage (*eglCreateImage)(EGLDisplay, EGLContext, EGLenum, void *, const intptr_t *);
  EGLBoolean (*eglDestroyImage)(EGLDisplay, EGLImage);
  void *(*(*eglGetProcAddress)(const char *))(void);
  EGLint (*eglGetError)(void);
  void (*img_to_rb)(GLenum, EGLImage);
  void (*glGenRenderbuffers)(GLsizei, GLuint *);
  void (*glBindRenderbuffer)(GLenum, GLuint);
  void (*glGenFramebuffers)(GLsizei, GLuint *);
  void (*glBindFramebuffer)(GLenum, GLuint);
  void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
  GLenum (*glCheckFramebufferStatus)(GLenum);
  void (*glFinish)(void);
  GLenum (*glGetError)(void);
  void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
  void (*glDeleteRenderbuffers)(GLsizei, const GLuint *);
  void (*glDeleteFramebuffers)(GLsizei, const GLuint *);
} G;

#define SYM(h, name) do { *(void **)&G.name = dlsym(h, #name); if (!G.name) { fprintf(stderr, "[aurora-wayland] gpuimp: falta %s\n", #name); return 0; } } while (0)

static int gpuimp_init(void) {
  setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/10_nvidia.json", 0);
  setenv("GBM_BACKENDS_PATH", "/usr/lib/x86_64-linux-gnu/gbm", 0);
  G.egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  G.gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
  G.gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!G.egl || !G.gles || !G.gbm) {
    fprintf(stderr, "[aurora-wayland] gpuimp: sin libEGL/libGLESv2/libgbm (%s)\n", dlerror());
    return 0;
  }
  *(void **)&G.gbm_create_device = dlsym(G.gbm, "gbm_create_device");
  if (!G.gbm_create_device)
    return 0;
  SYM(G.egl, eglGetPlatformDisplay); SYM(G.egl, eglInitialize); SYM(G.egl, eglBindAPI);
  SYM(G.egl, eglChooseConfig); SYM(G.egl, eglCreateContext); SYM(G.egl, eglMakeCurrent);
  SYM(G.egl, eglCreateImage); SYM(G.egl, eglDestroyImage); SYM(G.egl, eglGetProcAddress);
  SYM(G.egl, eglGetError);
  SYM(G.gles, glGenRenderbuffers); SYM(G.gles, glBindRenderbuffer); SYM(G.gles, glGenFramebuffers);
  SYM(G.gles, glBindFramebuffer); SYM(G.gles, glFramebufferRenderbuffer);
  SYM(G.gles, glCheckFramebufferStatus); SYM(G.gles, glFinish); SYM(G.gles, glGetError); SYM(G.gles, glReadPixels);
  SYM(G.gles, glDeleteRenderbuffers); SYM(G.gles, glDeleteFramebuffers);
  int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "[aurora-wayland] gpuimp: no abre renderD128\n");
    return 0;
  }
  void *gbm = G.gbm_create_device(fd);
  if (!gbm) {
    fprintf(stderr, "[aurora-wayland] gpuimp: gbm_create_device fallo\n");
    return 0;
  }
  G.dpy = G.eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
  EGLint maj, min;
  if (!G.dpy || !G.eglInitialize(G.dpy, &maj, &min)) {
    fprintf(stderr, "[aurora-wayland] gpuimp: eglInitialize fallo (0x%x)\n", G.eglGetError());
    return 0;
  }
  G.eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg;
  EGLint n = 0, ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  if (!G.eglChooseConfig(G.dpy, ca, &cfg, 1, &n) || n < 1)
    return 0;
  EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = G.eglCreateContext(G.dpy, cfg, NULL, xa);
  if (!ctx || !G.eglMakeCurrent(G.dpy, NULL, NULL, ctx)) {
    fprintf(stderr, "[aurora-wayland] gpuimp: contexto GLES2 fallo (0x%x)\n", G.eglGetError());
    return 0;
  }
  G.img_to_rb = (void (*)(GLenum, EGLImage))G.eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
  if (!G.img_to_rb)
    return 0;
  fprintf(stderr, "[aurora-wayland] gpuimp: EGL %d.%d listo (lectura de dmabufs de GPU)\n", maj, min);
  return 1;
}

struct gpuimp_h {
  EGLImage img;
  GLuint rb, fb;
  int w, h, bad;
};

static double gi_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void gpuimp_free_res(struct gpuimp_h *h) {
  if (h->fb) G.glDeleteFramebuffers(1, &h->fb);
  if (h->rb) G.glDeleteRenderbuffers(1, &h->rb);
  if (h->img) G.eglDestroyImage(G.dpy, h->img);
  h->fb = h->rb = 0;
  h->img = NULL;
}

/* Libera el import cacheado de un buffer (al destruirse el wl_buffer). */
void gpuimp_release(void *hp) {
  if (hp && G.ok) {
    gpuimp_free_res(hp);
    free(hp);
  }
}

/* El import (EGLImage + renderbuffer + FBO) se crea una vez por wl_buffer y se reutiliza en
 * cada cuadro: antes se creaba y destruia en cada commit. GPUIMP_NOCACHE=1 lo desactiva. */
const uint8_t *gpuimp_read(void **hp, int fd, int w, int h, uint32_t stride, uint32_t offset,
                           uint32_t fourcc, uint64_t modifier) {
  static int nocache = -1, bgra = -1, nfr;
  static double t_read, t_swz, t_imp;
  if (!G.tried) {
    G.tried = 1;
    fprintf(stderr, "[aurora-wayland] gpuimp: iniciando EGL...\n");
    G.ok = gpuimp_init();
  }
  if (!G.ok)
    return NULL;
  if (nocache < 0) {
    const char *e = getenv("GPUIMP_NOCACHE");
    nocache = e && *e == '1';
    fprintf(stderr, "[aurora-wayland] gpuimp: cache de imports %s\n", nocache ? "desactivada" : "activa");
  }
  struct gpuimp_h *H = hp && !nocache ? *hp : NULL;
  struct gpuimp_h tmp;
  if (H && H->bad)
    return NULL;
  double t0 = gi_now();
  if (!H) {
    if (G.fails < 5 && !G.logged_ok)
      fprintf(stderr, "[aurora-wayland] gpuimp: eglCreateImage %dx%d...\n", w, h);
    intptr_t at[] = {0x3057, w, 0x3056, h, 0x3271, fourcc, 0x3272, fd, 0x3273, offset,
                     0x3274, stride, 0x3443, (intptr_t)(modifier & 0xffffffffu),
                     0x3444, (intptr_t)(modifier >> 32), EGL_NONE};
    EGLImage img = G.eglCreateImage(G.dpy, NULL, 0x3270, NULL, at);
    if (!img) {
      if (G.fails++ < 5)
        fprintf(stderr, "[aurora-wayland] gpuimp: eglCreateImage %dx%d mod=0x%llx fallo (0x%x)\n", w, h,
                (unsigned long long)modifier, G.eglGetError());
      return NULL;
    }
    struct gpuimp_h *n = (hp && !nocache) ? calloc(1, sizeof(*n)) : &tmp;
    if (!n)
      n = &tmp;
    memset(n, 0, sizeof(*n));
    n->img = img; n->w = w; n->h = h;
    G.glGenRenderbuffers(1, &n->rb);
    G.glBindRenderbuffer(GL_RENDERBUFFER, n->rb);
    G.img_to_rb(GL_RENDERBUFFER, img);
    G.glGenFramebuffers(1, &n->fb);
    G.glBindFramebuffer(GL_FRAMEBUFFER, n->fb);
    G.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, n->rb);
    if (G.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      if (G.fails++ < 5)
        fprintf(stderr, "[aurora-wayland] gpuimp: framebuffer incompleto\n");
      n->bad = 1;
    }
    if (n != &tmp)
      *hp = n;
    H = n;
    t_imp += gi_now() - t0;
    t0 = gi_now();
    if (H->bad) {
      if (H == &tmp) {
        G.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gpuimp_free_res(&tmp);
      }
      return NULL;
    }
  } else {
    G.glBindFramebuffer(GL_FRAMEBUFFER, H->fb);
  }
  const uint8_t *res = NULL;
  size_t need = (size_t)w * h * 4;
  if (G.buf_len < need) {
    free(G.buf);
    G.buf = malloc(need);
    G.buf_len = G.buf ? need : 0;
  }
  if (G.buf) {
    G.glFinish();
    int swz = 1;
    if (bgra != 0) {
      G.glGetError();
      G.glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, G.buf);
      if (G.glGetError() == 0) swz = 0;
      if (bgra < 0) {
        bgra = !swz;
        fprintf(stderr, "[aurora-wayland] gpuimp: lectura BGRA directa %s\n", bgra ? "si" : "no (se intercambia R/B por CPU)");
      }
    }
    double t1 = gi_now();
    if (swz) {
      G.glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, G.buf);
      t1 = gi_now();
      for (size_t i = 0; i < need; i += 4) {
        uint8_t r = G.buf[i];
        G.buf[i] = G.buf[i + 2];
        G.buf[i + 2] = r;
      }
    }
    double t2 = gi_now();
    t_read += t1 - t0; t_swz += t2 - t1;
    res = G.buf;
    if (!G.logged_ok++)
      fprintf(stderr, "[aurora-wayland] gpuimp: primer frame leido de la GPU %dx%d\n", w, h);
    if (++nfr % 120 == 0)
      fprintf(stderr, "[aurora-wayland] gpuimp: %d cuadros %dx%d: lectura %.2f ms, swizzle %.2f ms, import %.2f ms (medias)\n",
              nfr, w, h, t_read / nfr, t_swz / nfr, t_imp / nfr);
  }
  if (H == &tmp) {
    G.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gpuimp_free_res(&tmp);
  }
  return res;
}

/* Modificadores que el EGL de la GPU acepta para renderizar en `fourcc` (los que un cliente
 * puede usar de verdad: se omiten los external_only). Devuelve cuantos. */
int gpuimp_query_modifiers(uint32_t fourcc, uint64_t *mods, int max) {
  if (!G.tried) {
    G.tried = 1;
    fprintf(stderr, "[aurora-wayland] gpuimp: iniciando EGL...\n");
    G.ok = gpuimp_init();
  }
  if (!G.ok || max <= 0)
    return 0;
  EGLBoolean (*q)(EGLDisplay, EGLint, EGLint, uint64_t *, EGLBoolean *, EGLint *) =
      (EGLBoolean (*)(EGLDisplay, EGLint, EGLint, uint64_t *, EGLBoolean *, EGLint *))
          G.eglGetProcAddress("eglQueryDmaBufModifiersEXT");
  if (!q) {
    fprintf(stderr, "[aurora-wayland] gpuimp: sin eglQueryDmaBufModifiersEXT\n");
    return 0;
  }
  uint64_t all[64];
  EGLBoolean ext[64];
  EGLint n = 0;
  if (!q(G.dpy, (EGLint)fourcc, 64, all, ext, &n) || n < 0) {
    fprintf(stderr, "[aurora-wayland] gpuimp: eglQueryDmaBufModifiersEXT fallo (0x%x)\n", G.eglGetError());
    return 0;
  }
  int out = 0;
  for (int i = 0; i < n && i < 64 && out < max; i++)
    if (!ext[i])
      mods[out++] = all[i];
  fprintf(stderr, "[aurora-wayland] gpuimp: fourcc 0x%x -> %d modificadores de render (de %d)\n", fourcc,
          out, (int)n);
  return out;
}
