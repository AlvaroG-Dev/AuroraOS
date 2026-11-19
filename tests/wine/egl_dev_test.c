// egl_dev_test: lo que hace win32u de Wine al arrancar: enumerar los
// dispositivos EGL (EGL_EXT_device_enumeration) e inicializar cada uno con
// EGL_PLATFORM_DEVICE_EXT. Con Mesa y LIBGL_ALWAYS_SOFTWARE=1 sobre un nodo
// render sin driver de Mesa (nvidia-drm), Mesa caia en drisw_init_screen.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>

int main(void) {
  void *m = dlopen("libEGL_mesa.so.0", RTLD_NOW);
  printf("EGL-DEV: libEGL_mesa %s\n", m ? "ok" : dlerror());
  void *h = dlopen("libEGL.so.1", RTLD_NOW);
  if (!h) { printf("EGL-DEV: sin libEGL (%s)\n", dlerror()); return 1; }
  PFNEGLGETPROCADDRESSPROC gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(h, "eglGetProcAddress");
  PFNEGLQUERYSTRINGPROC qstr = (PFNEGLQUERYSTRINGPROC)dlsym(h, "eglQueryString");
  const char *ce = qstr ? qstr(EGL_NO_DISPLAY, EGL_EXTENSIONS) : NULL;
  printf("EGL-DEV: cliente: %s\n", ce ? ce : "-");
  PFNEGLQUERYDEVICESEXTPROC qd = (PFNEGLQUERYDEVICESEXTPROC)gpa("eglQueryDevicesEXT");
  PFNEGLQUERYDEVICESTRINGEXTPROC qs = (PFNEGLQUERYDEVICESTRINGEXTPROC)gpa("eglQueryDeviceStringEXT");
  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)gpa("eglGetPlatformDisplayEXT");
  PFNEGLINITIALIZEPROC init = (PFNEGLINITIALIZEPROC)dlsym(h, "eglInitialize");
  if (!qd || !gpd || !init) { printf("EGL-DEV: sin EGL_EXT_device_enumeration (%p %p %p %p)\n", (void *)qd, (void *)qs, (void *)gpd, (void *)init); return 1; }
  EGLDeviceEXT dev[16];
  EGLint n = 0;
  qd(16, dev, &n);
  printf("EGL-DEV: %d dispositivos\n", n);
  for (int i = 0; i < n; i++) {
    const char *ext = qs ? qs(dev[i], EGL_EXTENSIONS) : NULL;
    const char *file = qs ? qs(dev[i], EGL_DRM_DEVICE_FILE_EXT) : NULL;
    printf("EGL-DEV: [%d] drm=%s ext=%s\n", i, file ? file : "-", ext ? ext : "-");
    fflush(stdout);
    EGLDisplay d = gpd(EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
    EGLint ma = 0, mi = 0;
    EGLBoolean ok = d != EGL_NO_DISPLAY && init(d, &ma, &mi);
    printf("EGL-DEV: [%d] eglInitialize %s %d.%d\n", i, ok ? "ok" : "fallo", ma, mi);
    fflush(stdout);
  }
  printf("EGL-DEV: fin\n");
  return 0;
}
