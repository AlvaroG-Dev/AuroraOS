// kernel/firmware.c
#include "firmware.h"
#include "heap.h"
#include "klog.h"
#include "string.h"
#include "test.h"
#include "uaccess.h"
#include "vfs.h"

#ifndef E2BIG
#define E2BIG 7
#endif

static const char *const g_fw_dirs[] = {"/lib/firmware/updates/", "/lib/firmware/"};

static int name_ok(const char *name) {
  if (!name || !name[0] || name[0] == '/' || strlen(name) > 200)
    return 0;
  for (const char *p = name; *p; p++)
    if (p[0] == '.' && p[1] == '.' && (p == name || p[-1] == '/') &&
        (p[2] == '/' || p[2] == 0))
      return 0; // sin escapar de /lib/firmware
  return 1;
}

int request_firmware(const struct firmware **out, const char *name) {
  if (!out)
    return -EINVAL;
  *out = NULL;
  if (!name_ok(name))
    return -EINVAL;
  for (unsigned d = 0; d < sizeof(g_fw_dirs) / sizeof(g_fw_dirs[0]); d++) {
    char path[300];
    snprintf(path, sizeof(path), "%s%s", g_fw_dirs[d], name);
    vfs_node_t *n = vfs_lookup(path);
    if (!n)
      continue;
    if ((n->mode & S_IFMT) != S_IFREG || !n->ops || !n->ops->read) {
      vfs_node_free(n);
      continue;
    }
    size_t sz = (size_t)n->size;
    if (sz > FIRMWARE_MAX_SIZE) {
      vfs_node_free(n);
      return -E2BIG;
    }
    struct firmware *fw = (struct firmware *)kzalloc(sizeof(*fw));
    uint8_t *buf = (uint8_t *)kmalloc(sz ? sz : 1);
    if (!fw || !buf) {
      kfree(fw);
      kfree(buf);
      vfs_node_free(n);
      return -ENOMEM;
    }
    size_t got = 0;
    while (got < sz) {
      size_t chunk = sz - got > (1u << 20) ? (1u << 20) : sz - got;
      int64_t r = n->ops->read(n, got, chunk, buf + got);
      if (r <= 0)
        break;
      got += (size_t)r;
    }
    vfs_node_free(n);
    if (got != sz) {
      kfree(fw);
      kfree(buf);
      return -EIO;
    }
    fw->size = sz;
    fw->data = buf;
    *out = fw;
    LOG_INFO("[FW] cargado %s (%lu bytes)", path, (unsigned long)sz);
    return 0;
  }
  return -ENOENT;
}

void release_firmware(const struct firmware *fw) {
  if (!fw)
    return;
  kfree((void *)fw->data);
  kfree((void *)fw);
}

// /lib/firmware/aurora/selftest.bin = "AURORA-FW\n" (ver precompiled/lib/firmware).
static void firmware_selftest(void) {
  const struct firmware *fw = NULL;
  TEST_ASSERT(request_firmware(&fw, "aurora/selftest.bin") == 0 && fw,
              "request_firmware encuentra aurora/selftest.bin");
  if (fw) {
    TEST_ASSERT(fw->size == 10 && memcmp(fw->data, "AURORA-FW\n", 10) == 0,
                "contenido del firmware correcto");
    release_firmware(fw);
  }
  TEST_ASSERT(request_firmware(&fw, "aurora/no-existe.bin") == -ENOENT,
              "firmware inexistente: -ENOENT");
  TEST_ASSERT(request_firmware(&fw, "../etc/passwd") == -EINVAL,
              "nombre con .. rechazado");
  TEST_ASSERT(request_firmware(&fw, "/etc/passwd") == -EINVAL,
              "ruta absoluta rechazada");
}

REGISTER_TEST("firmware: request_firmware", firmware_selftest);
