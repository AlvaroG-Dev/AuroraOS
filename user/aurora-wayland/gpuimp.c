#define _GNU_SOURCE
#include "gpuimp.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
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
  SYM(G.gles, glCheckFramebufferStatus); SYM(G.gles, glFinish); SYM(G.gles, glReadPixels);
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

const uint8_t *gpuimp_read(int fd, int w, int h, uint32_t stride, uint32_t offset,
                           uint32_t fourcc, uint64_t modifier) {
  if (!G.tried) {
    G.tried = 1;
    G.ok = gpuimp_init();
  }
  if (!G.ok)
    return NULL;
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
  G.glGenRenderbuffers(1, &G.rb);
  G.glBindRenderbuffer(GL_RENDERBUFFER, G.rb);
  G.img_to_rb(GL_RENDERBUFFER, img);
  G.glGenFramebuffers(1, &G.fb);
  G.glBindFramebuffer(GL_FRAMEBUFFER, G.fb);
  G.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, G.rb);
  const uint8_t *res = NULL;
  if (G.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    size_t need = (size_t)w * h * 4;
    if (G.buf_len < need) {
      free(G.buf);
      G.buf = malloc(need);
      G.buf_len = G.buf ? need : 0;
    }
    if (G.buf) {
      G.glFinish();
      G.glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, G.buf);
      /* RGBA -> BGRA (memoria de XRGB8888) */
      for (size_t i = 0; i < need; i += 4) {
        uint8_t r = G.buf[i];
        G.buf[i] = G.buf[i + 2];
        G.buf[i + 2] = r;
      }
      res = G.buf;
      if (!G.logged_ok++)
        fprintf(stderr, "[aurora-wayland] gpuimp: primer frame leido de la GPU %dx%d\n", w, h);
    }
  } else if (G.fails++ < 5) {
    fprintf(stderr, "[aurora-wayland] gpuimp: framebuffer incompleto\n");
  }
  G.glBindFramebuffer(GL_FRAMEBUFFER, 0);
  G.glDeleteFramebuffers(1, &G.fb);
  G.glDeleteRenderbuffers(1, &G.rb);
  G.eglDestroyImage(G.dpy, img);
  return res;
}
