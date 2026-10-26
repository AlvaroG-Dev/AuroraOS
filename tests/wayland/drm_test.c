// tests/wayland/drm_test.c
//
// Prueba de /dev/dri/card0 con las cabeceras uapi reales de Linux (sin
// libdrm): version, caps, recursos KMS, dumb buffer + mmap, ADDFB2, SETCRTC,
// PAGE_FLIP con evento, propiedades y commit atomico, y limpieza.

#include <drm/drm.h>
#include <linux/dma-buf.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      printf("[drm_test] PASS " __VA_ARGS__);                                  \
      printf("\n");                                                            \
    } else {                                                                   \
      printf("[drm_test] FAIL " __VA_ARGS__);                                  \
      printf(" (errno=%d)\n", errno);                                          \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

static uint32_t find_prop(int fd, uint32_t obj, uint32_t type, const char *name) {
  struct drm_mode_obj_get_properties g = {.obj_id = obj, .obj_type = type};
  if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) < 0)
    return 0;
  uint32_t ids[32];
  uint64_t vals[32];
  if (g.count_props > 32)
    return 0;
  g.props_ptr = (uintptr_t)ids;
  g.prop_values_ptr = (uintptr_t)vals;
  if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) < 0)
    return 0;
  for (uint32_t i = 0; i < g.count_props; i++) {
    struct drm_mode_get_property p = {.prop_id = ids[i]};
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0)
      continue;
    if (!strcmp(p.name, name))
      return ids[i];
  }
  return 0;
}

static int wait_event(int fd, uint32_t type, uint64_t user_data) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  if (poll(&pfd, 1, 2000) <= 0)
    return 0;
  char buf[128];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n < (ssize_t)sizeof(struct drm_event_vblank))
    return 0;
  struct drm_event_vblank *e = (struct drm_event_vblank *)buf;
  return e->base.type == type && e->user_data == user_data;
}

int main(int argc, char **argv) {
  (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  CHECK(fd >= 0, "open /dev/dri/card0");
  if (fd < 0)
    return 1;

  // VERSION (dos pasadas, como libdrm)
  struct drm_version v;
  memset(&v, 0, sizeof(v));
  CHECK(ioctl(fd, DRM_IOCTL_VERSION, &v) == 0 && v.name_len > 0, "VERSION len");
  char name[32] = {0};
  v.name = name;
  v.name_len = sizeof(name) - 1;
  v.date = NULL; v.date_len = 0; v.desc = NULL; v.desc_len = 0;
  CHECK(ioctl(fd, DRM_IOCTL_VERSION, &v) == 0 && !strcmp(name, "aurora"),
        "VERSION name=%s", name);

  struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
  CHECK(ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1, "CAP_DUMB_BUFFER");
  struct drm_set_client_cap cc = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
  CHECK(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cc) == 0, "CLIENT_CAP universal planes");
  cc.capability = DRM_CLIENT_CAP_ATOMIC;
  CHECK(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cc) == 0, "CLIENT_CAP atomic");

  // Recursos
  struct drm_mode_card_res res;
  memset(&res, 0, sizeof(res));
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 &&
            res.count_crtcs == 1 && res.count_connectors == 1 &&
            res.count_encoders == 1,
        "GETRESOURCES crtcs=%u conns=%u encs=%u", res.count_crtcs,
        res.count_connectors, res.count_encoders);
  uint32_t crtc = 0, conn = 0, enc = 0;
  res.crtc_id_ptr = (uintptr_t)&crtc;
  res.connector_id_ptr = (uintptr_t)&conn;
  res.encoder_id_ptr = (uintptr_t)&enc;
  res.fb_id_ptr = 0;
  res.count_fbs = 0;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 && crtc && conn && enc,
        "GETRESOURCES ids crtc=%u conn=%u enc=%u", crtc, conn, enc);

  // Conector: dos pasadas
  struct drm_mode_get_connector gc;
  memset(&gc, 0, sizeof(gc));
  gc.connector_id = conn;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) == 0 && gc.count_modes >= 1 &&
            gc.connection == 1,
        "GETCONNECTOR modes=%u props=%u", gc.count_modes, gc.count_props);
  struct drm_mode_modeinfo modes[16];
  uint32_t cprops[8];
  uint64_t cvals[8];
  uint32_t cenc[2];
  if (gc.count_modes > 16 || gc.count_props > 8 || gc.count_encoders > 2)
    return 1;
  gc.modes_ptr = (uintptr_t)modes;
  gc.props_ptr = (uintptr_t)cprops;
  gc.prop_values_ptr = (uintptr_t)cvals;
  gc.encoders_ptr = (uintptr_t)cenc;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) == 0, "GETCONNECTOR fill");
  struct drm_mode_modeinfo mode = modes[0];
  CHECK(mode.hdisplay >= 64 && (mode.type & DRM_MODE_TYPE_PREFERRED),
        "preferred mode %s (%ux%u)", mode.name, mode.hdisplay, mode.vdisplay);

  struct drm_mode_get_encoder ge = {.encoder_id = enc};
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0 && ge.possible_crtcs == 1,
        "GETENCODER");

  // Planos
  struct drm_mode_get_plane_res pr;
  memset(&pr, 0, sizeof(pr));
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) == 0 && pr.count_planes == 1,
        "GETPLANERESOURCES n=%u", pr.count_planes);
  uint32_t plane = 0;
  pr.plane_id_ptr = (uintptr_t)&plane;
  ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
  CHECK(plane != 0, "plane id=%u", plane);

  // Dumb buffer
  struct drm_mode_create_dumb cd = {.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
  CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0 && cd.handle && cd.pitch >= cd.width * 4,
        "CREATE_DUMB %ux%u pitch=%u size=%llu", cd.width, cd.height, cd.pitch,
        (unsigned long long)cd.size);
  struct drm_mode_map_dumb md = {.handle = cd.handle};
  CHECK(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0, "MAP_DUMB offset=%llx",
        (unsigned long long)md.offset);
  uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
  CHECK(px != MAP_FAILED, "mmap dumb");
  if (px == MAP_FAILED)
    return 1;
  for (uint32_t y = 0; y < cd.height; y++)
    for (uint32_t x = 0; x < cd.width; x++)
      px[y * (cd.pitch / 4) + x] = ((x / 64 + y / 64) & 1) ? 0x2060C0 : 0xE0A030;
  CHECK(px[0] == 0xE0A030 && px[64] == 0x2060C0, "escritura/lectura del mapeo");

  // ADDFB2
  struct drm_mode_fb_cmd2 fb2;
  memset(&fb2, 0, sizeof(fb2));
  fb2.width = cd.width;
  fb2.height = cd.height;
  fb2.pixel_format = DRM_FORMAT_XRGB8888;
  fb2.handles[0] = cd.handle;
  fb2.pitches[0] = cd.pitch;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) == 0 && fb2.fb_id, "ADDFB2 fb=%u", fb2.fb_id);

  // SETCRTC (legacy)
  struct drm_mode_crtc sc;
  memset(&sc, 0, sizeof(sc));
  sc.crtc_id = crtc;
  sc.fb_id = fb2.fb_id;
  sc.set_connectors_ptr = (uintptr_t)&conn;
  sc.count_connectors = 1;
  sc.mode_valid = 1;
  sc.mode = mode;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc) == 0, "SETCRTC");
  struct drm_mode_crtc gcr = {.crtc_id = crtc};
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &gcr) == 0 && gcr.fb_id == fb2.fb_id &&
            gcr.mode_valid && gcr.mode.hdisplay == mode.hdisplay,
        "GETCRTC devuelve el modo activo");

  // Segundo buffer + PAGE_FLIP con evento
  struct drm_mode_create_dumb cd2 = cd;
  cd2.handle = 0;
  cd2.pitch = 0;
  cd2.size = 0;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd2) == 0, "CREATE_DUMB #2");
  struct drm_mode_fb_cmd2 fbb = fb2;
  fbb.fb_id = 0;
  fbb.handles[0] = cd2.handle;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fbb) == 0, "ADDFB2 #2");
  struct drm_mode_crtc_page_flip pf = {.crtc_id = crtc, .fb_id = fbb.fb_id,
                                       .flags = DRM_MODE_PAGE_FLIP_EVENT,
                                       .user_data = 0xABCD};
  CHECK(ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0, "PAGE_FLIP");
  CHECK(wait_event(fd, DRM_EVENT_FLIP_COMPLETE, 0xABCD), "evento FLIP_COMPLETE");
  pf.fb_id = fb2.fb_id;
  pf.user_data = 0x1234;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0, "PAGE_FLIP de vuelta");
  CHECK(wait_event(fd, DRM_EVENT_FLIP_COMPLETE, 0x1234), "evento FLIP_COMPLETE #2");

  // Atomic
  uint32_t p_fb = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
  uint32_t p_pcrtc = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
  uint32_t p_active = find_prop(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
  uint32_t p_mode = find_prop(fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
  uint32_t p_ccrtc = find_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
  uint32_t p_sw = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
  uint32_t p_sh = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
  uint32_t p_cw = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
  uint32_t p_ch = find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
  CHECK(p_fb && p_pcrtc && p_active && p_mode && p_ccrtc && p_sw && p_sh && p_cw && p_ch,
        "propiedades atomic encontradas");

  struct drm_mode_create_blob cb = {.data = (uintptr_t)&mode, .length = sizeof(mode)};
  CHECK(ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb) == 0 && cb.blob_id, "CREATEPROPBLOB");
  struct drm_mode_get_blob gb = {.blob_id = cb.blob_id};
  struct drm_mode_modeinfo back;
  gb.length = sizeof(back);
  gb.data = (uintptr_t)&back;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb) == 0 && !memcmp(&back, &mode, sizeof(mode)),
        "GETPROPBLOB coincide");

  uint32_t objs[3] = {plane, crtc, conn};
  uint32_t counts[3] = {7, 2, 1};
  uint32_t props[10] = {p_fb, p_pcrtc, p_sw, p_sh, p_cw, p_ch, find_prop(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X"),
                        p_active, p_mode, p_ccrtc};
  uint64_t vals[10] = {fb2.fb_id, crtc, (uint64_t)mode.hdisplay << 16, (uint64_t)mode.vdisplay << 16,
                       mode.hdisplay, mode.vdisplay, 0, 1, cb.blob_id, crtc};
  struct drm_mode_atomic at;
  memset(&at, 0, sizeof(at));
  at.flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
  at.count_objs = 3;
  at.objs_ptr = (uintptr_t)objs;
  at.count_props_ptr = (uintptr_t)counts;
  at.props_ptr = (uintptr_t)props;
  at.prop_values_ptr = (uintptr_t)vals;
  at.user_data = 0x77;
  at.flags |= DRM_MODE_ATOMIC_TEST_ONLY;
  at.flags &= ~DRM_MODE_PAGE_FLIP_EVENT;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at) == 0, "ATOMIC test-only");
  at.flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at) == 0, "ATOMIC commit");
  CHECK(wait_event(fd, DRM_EVENT_FLIP_COMPLETE, 0x77), "evento atomic");
  if (argc > 1) { // "hold": deja el patron en pantalla para una captura
    printf("[drm_test] HOLD\n");
    sleep(8);
  }

  // Commit invalido: fb inexistente
  vals[0] = 9999;
  at.flags = DRM_MODE_ATOMIC_TEST_ONLY;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at) < 0, "ATOMIC con fb invalido falla");

  // Apagar: ACTIVE=0, sin plano ni conector
  uint32_t off_objs[3] = {plane, crtc, conn};
  uint32_t off_counts[3] = {2, 2, 1};
  uint32_t off_props[5] = {p_fb, p_pcrtc, p_active, p_mode, p_ccrtc};
  uint64_t off_vals[5] = {0, 0, 0, 0, 0};
  memset(&at, 0, sizeof(at));
  at.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  at.count_objs = 3;
  at.objs_ptr = (uintptr_t)off_objs;
  at.count_props_ptr = (uintptr_t)off_counts;
  at.props_ptr = (uintptr_t)off_props;
  at.prop_values_ptr = (uintptr_t)off_vals;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at) == 0, "ATOMIC apagar");
  gcr.mode_valid = 0;
  CHECK(ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &gcr) == 0 && gcr.fb_id == 0, "CRTC apagado");

  // Un segundo fd no es master
  int fd2 = open("/dev/dri/card0", O_RDWR);
  struct drm_mode_crtc bad = sc;
  CHECK(fd2 >= 0 && ioctl(fd2, DRM_IOCTL_MODE_SETCRTC, &bad) < 0 && errno == EACCES,
        "segundo fd no puede hacer modeset");
  close(fd2);

  // Render node: sin KMS
  int rfd = open("/dev/dri/renderD128", O_RDWR);
  struct drm_mode_card_res rr;
  memset(&rr, 0, sizeof(rr));
  CHECK(rfd >= 0 && ioctl(rfd, DRM_IOCTL_MODE_GETRESOURCES, &rr) < 0, "renderD128 rechaza KMS");
  close(rfd);

  // PRIME / dma-buf: exportar el dumb, mapear el fd, reimportar en otro fichero
  // DRM (render node) y compartir las mismas paginas sin copia.
  struct drm_get_cap pcap = {.capability = DRM_CAP_PRIME};
  CHECK(ioctl(fd, DRM_IOCTL_GET_CAP, &pcap) == 0 &&
            (pcap.value & (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) ==
                (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT),
        "CAP_PRIME import|export (=%llu)", (unsigned long long)pcap.value);
  struct drm_prime_handle ph = {.handle = cd.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
  CHECK(ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0 && ph.fd >= 0,
        "PRIME_HANDLE_TO_FD fd=%d", ph.fd);
  int dbuf = ph.fd;
  CHECK(lseek(dbuf, 0, SEEK_END) == (off_t)cd.size, "lseek(dmabuf, SEEK_END) = size");
  uint32_t *dpx = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, dbuf, 0);
  CHECK(dpx != MAP_FAILED && dpx[0] == 0xE0A030 && dpx[64] == 0x2060C0,
        "mmap del dmabuf ve el contenido del dumb");
  if (dpx != MAP_FAILED) {
    dpx[1] = 0xCAFE01;
    CHECK(px[1] == 0xCAFE01, "dmabuf y dumb comparten paginas");
    munmap(dpx, cd.size);
  }
  struct drm_prime_handle same = {.fd = dbuf};
  CHECK(ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &same) == 0 && same.handle == cd.handle,
        "FD_TO_HANDLE en el mismo fichero devuelve el handle original");
  int rfd2 = open("/dev/dri/renderD128", O_RDWR);
  struct drm_prime_handle imp = {.fd = dbuf};
  CHECK(rfd2 >= 0 && ioctl(rfd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &imp) == 0 && imp.handle,
        "FD_TO_HANDLE en renderD128 handle=%u", imp.handle);
  struct drm_mode_map_dumb imd = {.handle = imp.handle};
  uint32_t *ipx = MAP_FAILED;
  if (ioctl(rfd2, DRM_IOCTL_MODE_MAP_DUMB, &imd) == 0)
    ipx = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, rfd2, imd.offset);
  CHECK(ipx != MAP_FAILED && ipx[0] == 0xE0A030 && ipx[1] == 0xCAFE01,
        "mmap del GEM importado ve el mismo contenido");
  if (ipx != MAP_FAILED)
    munmap(ipx, cd.size);
  struct drm_prime_handle bad_fd = {.fd = 0};
  CHECK(ioctl(rfd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &bad_fd) < 0, "FD_TO_HANDLE con fd que no es dmabuf falla");
  struct drm_gem_close gcl = {.handle = imp.handle};
  CHECK(ioctl(rfd2, DRM_IOCTL_GEM_CLOSE, &gcl) == 0, "GEM_CLOSE del importado");
  close(rfd2);
  close(dbuf);

  // GEM_FLINK / GEM_OPEN y fences implicitos del dma-buf (sync_file).
  {
    struct drm_gem_flink fl = {.handle = cd.handle};
    CHECK(ioctl(fd, DRM_IOCTL_GEM_FLINK, &fl) == 0 && fl.name,
          "GEM_FLINK da un nombre global (%u)", fl.name);
    struct drm_gem_flink fl2 = {.handle = cd.handle};
    CHECK(ioctl(fd, DRM_IOCTL_GEM_FLINK, &fl2) == 0 && fl2.name == fl.name,
          "GEM_FLINK repetido devuelve el mismo nombre");
    int card2 = open("/dev/dri/card0", O_RDWR);
    struct drm_gem_open op = {.name = fl.name};
    CHECK(card2 >= 0 && ioctl(card2, DRM_IOCTL_GEM_OPEN, &op) == 0 && op.handle && op.size >= cd.size,
          "GEM_OPEN en otro fichero (handle=%u size=%llu)", op.handle, (unsigned long long)op.size);
    struct drm_mode_map_dumb omd = {.handle = op.handle};
    uint32_t *opx = MAP_FAILED;
    if (ioctl(card2, DRM_IOCTL_MODE_MAP_DUMB, &omd) == 0)
      opx = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, card2, omd.offset);
    CHECK(opx != MAP_FAILED && opx[0] == 0xE0A030, "mmap del GEM abierto por nombre ve el contenido");
    if (opx != MAP_FAILED)
      munmap(opx, cd.size);
    struct drm_gem_open bad = {.name = 0xFFFFFF};
    CHECK(ioctl(card2, DRM_IOCTL_GEM_OPEN, &bad) < 0, "GEM_OPEN con nombre inexistente falla");
    int rnode = open("/dev/dri/renderD128", O_RDWR);
    struct drm_gem_flink rf = {.handle = 1};
    CHECK(rnode >= 0 && ioctl(rnode, DRM_IOCTL_GEM_FLINK, &rf) < 0,
          "GEM_FLINK no esta permitido en renderD128");
    close(rnode);
    struct drm_gem_close gc2 = {.handle = op.handle};
    CHECK(ioctl(card2, DRM_IOCTL_GEM_CLOSE, &gc2) == 0, "GEM_CLOSE del abierto");
    close(card2);

    // dma-buf: fence implicito
    struct drm_prime_handle dph = {.handle = cd.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
    CHECK(ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph) == 0, "PRIME para fences implicitos");
    struct dma_buf_export_sync_file ex = {.flags = DMA_BUF_SYNC_RW, .fd = -1};
    CHECK(ioctl(dph.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &ex) == 0 && ex.fd >= 0,
          "EXPORT_SYNC_FILE fd=%d", ex.fd);
    struct pollfd dpf = {.fd = ex.fd, .events = POLLIN};
    CHECK(poll(&dpf, 1, 0) == 1, "el fence inicial del dma-buf esta senalado");
    close(ex.fd);

    // Un syncobj sin senalar importado como fence implicito bloquea el dma-buf.
    int sfd = open("/dev/dri/renderD128", O_RDWR);
    struct drm_syncobj_create nc = {0};
    ioctl(sfd, DRM_IOCTL_SYNCOBJ_CREATE, &nc);
    struct drm_syncobj_handle nh = {.handle = nc.handle,
                                    .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE};
    ioctl(sfd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &nh);
    struct dma_buf_import_sync_file im = {.flags = DMA_BUF_SYNC_WRITE, .fd = nh.fd};
    CHECK(ioctl(dph.fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &im) == 0, "IMPORT_SYNC_FILE");
    struct pollfd bpf = {.fd = dph.fd, .events = POLLIN};
    CHECK(poll(&bpf, 1, 0) == 0, "dma-buf con fence pendiente: poll no esta listo");
    struct drm_syncobj_array sg = {.handles = (uintptr_t)&nc.handle, .count_handles = 1};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_SIGNAL, &sg) == 0, "senalar el fence del productor");
    CHECK(poll(&bpf, 1, 1000) == 1 && (bpf.revents & POLLIN), "dma-buf listo tras senalar");
    struct dma_buf_export_sync_file ex2 = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
    CHECK(ioctl(dph.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &ex2) == 0 && ex2.fd >= 0, "EXPORT tras importar");
    if (ex2.fd >= 0) close(ex2.fd);
    struct dma_buf_export_sync_file bad0 = {.flags = 0, .fd = -1};
    CHECK(ioctl(dph.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &bad0) < 0, "EXPORT sin flags falla");
    close(nh.fd);
    close(sfd);
    close(dph.fd);
  }

  // drm_syncobj / sync_file: caps, binario, linea temporal, fd y despertar
  // entre procesos.
  {
    struct drm_get_cap sc = {.capability = DRM_CAP_SYNCOBJ};
    struct drm_get_cap tc = {.capability = DRM_CAP_SYNCOBJ_TIMELINE};
    CHECK(ioctl(fd, DRM_IOCTL_GET_CAP, &sc) == 0 && sc.value == 1 &&
              ioctl(fd, DRM_IOCTL_GET_CAP, &tc) == 0 && tc.value == 1,
          "CAP_SYNCOBJ y CAP_SYNCOBJ_TIMELINE");
    int sfd = open("/dev/dri/renderD128", O_RDWR); // tambien en el nodo render
    struct drm_syncobj_create c0 = {0}, c1 = {.flags = DRM_SYNCOBJ_CREATE_SIGNALED};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_CREATE, &c0) == 0 && c0.handle &&
              ioctl(sfd, DRM_IOCTL_SYNCOBJ_CREATE, &c1) == 0 && c1.handle,
          "SYNCOBJ_CREATE (sin senalar y senalado) %u,%u", c0.handle, c1.handle);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    uint32_t h0 = c0.handle, h1 = c1.handle;
    struct drm_syncobj_wait w = {.handles = (uintptr_t)&h1, .count_handles = 1,
                                 .timeout_nsec = now};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_WAIT, &w) == 0, "WAIT de un syncobj ya senalado");
    w.handles = (uintptr_t)&h0;
    w.timeout_nsec = now + 50000000LL;
    errno = 0;
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_WAIT, &w) < 0 && errno == ETIME,
          "WAIT sin senalar vence con ETIME (errno=%d)", errno);
    struct drm_syncobj_array arr = {.handles = (uintptr_t)&h0, .count_handles = 1};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_SIGNAL, &arr) == 0, "SYNCOBJ_SIGNAL");
    w.timeout_nsec = now;
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_WAIT, &w) == 0, "WAIT tras SIGNAL");
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_RESET, &arr) == 0, "SYNCOBJ_RESET");
    w.timeout_nsec = now;
    errno = 0;
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_WAIT, &w) < 0 && errno == ETIME, "WAIT tras RESET vence");

    // Linea temporal
    uint64_t pt5 = 5, q = 0;
    struct drm_syncobj_timeline_array tl = {.handles = (uintptr_t)&h0, .points = (uintptr_t)&pt5,
                                            .count_handles = 1};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &tl) == 0, "TIMELINE_SIGNAL punto 5");
    struct drm_syncobj_timeline_array qa = {.handles = (uintptr_t)&h0, .points = (uintptr_t)&q,
                                            .count_handles = 1};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_QUERY, &qa) == 0 && q == 5, "SYNCOBJ_QUERY = %llu",
          (unsigned long long)q);
    uint64_t p3 = 3, p7 = 7;
    struct drm_syncobj_timeline_wait tw = {.handles = (uintptr_t)&h0, .points = (uintptr_t)&p3,
                                           .count_handles = 1, .timeout_nsec = now};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &tw) == 0, "TIMELINE_WAIT punto 3 (<=5)");
    tw.points = (uintptr_t)&p7;
    tw.timeout_nsec = now + 30000000LL;
    errno = 0;
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &tw) < 0 && errno == ETIME,
          "TIMELINE_WAIT punto 7 vence");

    // fd del syncobj y sync_file
    struct drm_syncobj_create c2 = {0};
    ioctl(sfd, DRM_IOCTL_SYNCOBJ_CREATE, &c2);
    struct drm_syncobj_handle hf = {.handle = c2.handle, .flags = DRM_CLOEXEC};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &hf) == 0 && hf.fd >= 0, "HANDLE_TO_FD fd=%d", hf.fd);
    struct drm_syncobj_handle sf = {.handle = c2.handle,
                                    .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sf) == 0 && sf.fd >= 0,
          "HANDLE_TO_FD como sync_file fd=%d", sf.fd);
    struct pollfd pf = {.fd = sf.fd, .events = POLLIN};
    CHECK(poll(&pf, 1, 0) == 0, "sync_file sin senalar: poll no esta listo");
    int sfd2 = open("/dev/dri/renderD128", O_RDWR);
    struct drm_syncobj_handle imp2 = {.fd = hf.fd};
    CHECK(ioctl(sfd2, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp2) == 0 && imp2.handle,
          "FD_TO_HANDLE en otro fichero DRM (objeto compartido)");

    pid_t pid = fork();
    if (pid == 0) { // hijo: senala tras 300 ms desde su propio fichero DRM
      struct timespec d = {0, 300000000};
      nanosleep(&d, NULL);
      uint32_t hh = imp2.handle;
      struct drm_syncobj_array a2 = {.handles = (uintptr_t)&hh, .count_handles = 1};
      _exit(ioctl(sfd2, DRM_IOCTL_SYNCOBJ_SIGNAL, &a2) == 0 ? 0 : 1);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t t0 = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    uint32_t h2 = c2.handle;
    struct drm_syncobj_wait bw = {.handles = (uintptr_t)&h2, .count_handles = 1,
                                  .timeout_nsec = t0 + 5000000000LL};
    int brc = ioctl(sfd, DRM_IOCTL_SYNCOBJ_WAIT, &bw);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t el = ((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec - t0) / 1000000;
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(brc == 0 && el >= 200 && el < 3000 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "WAIT bloqueante despertado por otro proceso (%lld ms)", (long long)el);
    CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "sync_file pasa a senalado (poll)");
    struct drm_syncobj_handle imp3 = {.fd = sf.fd,
                                      .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE};
    uint32_t h3;
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp3) == 0 && (h3 = imp3.handle), "import de sync_file");
    struct drm_syncobj_wait w3 = {.handles = (uintptr_t)&h3, .count_handles = 1, .timeout_nsec = now};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_WAIT, &w3) == 0, "syncobj importado de sync_file senalado");
    struct drm_syncobj_destroy dsy = {.handle = c2.handle};
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_DESTROY, &dsy) == 0, "SYNCOBJ_DESTROY");
    errno = 0;
    CHECK(ioctl(sfd, DRM_IOCTL_SYNCOBJ_DESTROY, &dsy) < 0, "DESTROY de handle ya destruido falla");
    close(sf.fd);
    close(hf.fd);
    close(sfd2);
    close(sfd);
  }

  // Limpieza
  CHECK(ioctl(fd, DRM_IOCTL_MODE_RMFB, &fbb.fb_id) == 0, "RMFB");
  CHECK(ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb2.fb_id) == 0, "RMFB #2");
  struct drm_mode_destroy_dumb dd = {.handle = cd.handle};
  CHECK(ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) == 0, "DESTROY_DUMB");
  CHECK(px[0] == 0xE0A030, "el mapeo sigue valido tras destroy (paginas diferidas)");
  munmap(px, cd.size);
  close(fd);

  printf("[drm_test] %s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
