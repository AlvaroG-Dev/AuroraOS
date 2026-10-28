// kernel/drm/kms_present.h
//
// Presentacion del compositor por KMS (L7 hito 2): un cliente DRM del kernel
// (lkpi_drm.kur, kernel/linuxkpi/drivers/lkpi_drm/kms_present.c) hace el
// modeset del panel con dos buffers dumb y registra aqui sus operaciones. El
// compositor pinta en el buffer trasero y lo presenta con un commit atomico
// (page-flip con vblank real). Sin backend registrado, el compositor sigue
// volcando al framebuffer del firmware (GOP).
#ifndef KMS_PRESENT_H
#define KMS_PRESENT_H

// Solo tipos primitivos: lo incluyen el kernel y los .kur de LinuxKPI.

struct drm_lx_kms_ops {
  const char *name; // driver DRM ("i915", "bochs-drm"...)
  int w, h;         // modo activo (debe coincidir con el del compositor)
  int hw_vblank;    // 1 si el CRTC tiene vblank real (contador de hardware)
  // Buffer trasero: puntero CPU (XRGB8888) y paso en pixeles.
  unsigned int *(*back)(unsigned int *pitch_px);
  // Presenta el buffer trasero y lo intercambia con el delantero. `dirty` es
  // el rectangulo escrito (x, y, w, h) que hay que volcar a memoria si el
  // driver lo necesita. Bloquea hasta que el flip se completa (vblank). El
  // primer flip hace el modeset del panel (el primer frame ya sale pintado).
  // vbl_gap: vblanks transcurridos desde el flip anterior (1 = sin perdidas,
  // 0 = desconocido). Devuelve 0 o -errno (el buffer no se intercambia);
  // -ENODEV = el backend no sirve (el compositor lo retira y vuelve al GOP).
  int (*flip)(const int dirty[4], unsigned int *commit_us, unsigned int *vbl_gap);
};

// Registro desde el modulo (tras un modeset correcto con el primer buffer).
int drm_lx_kms_register(const struct drm_lx_kms_ops *ops);
void drm_lx_kms_unregister(const struct drm_lx_kms_ops *ops);
// Resolucion del compositor (el modulo solo acepta un modo de este tamano).
void drm_lx_kms_screen_size(int *w, int *h);

// Uso por el compositor. begin() toma el backend (NULL si no hay o el tamano
// no coincide) y lo retiene hasta end(); se llama desde el hilo del compositor.
const struct drm_lx_kms_ops *drm_lx_kms_begin(int w, int h);
void drm_lx_kms_end(void);
// Generacion del backend: cambia al registrarlo o quitarlo (el compositor
// repinta entero y reinicia su historial).
unsigned int drm_lx_kms_generation(void);
// Solo desde el compositor, entre begin() y end(): retira el backend.
void drm_lx_kms_dead(void);

#endif
