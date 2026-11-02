// tests/nvidia/nvctl_test.c
//
// Primer contacto de usuario con el RM de NVIDIA a traves de /dev/nvidiactl con
// las cabeceras uapi reales del driver (sin libnvidia): version de la API,
// tarjetas detectadas, cliente raiz (NV01_ROOT_CLIENT), GPUs conectadas
// (NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS) y liberacion del cliente.
// Se compila en el host con scripts/nvidia-build-test.sh (make nvidia).

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nvtypes.h"
#include "nv-ioctl.h"
#include "nv-ioctl-numbers.h"
#include "nvos.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"

#define NV_ESC_RM_FREE    0x29
#define NV_ESC_RM_CONTROL 0x2A
#define NV_ESC_RM_ALLOC   0x2B
#define NV_MAX_CARDS      32

static int g_fail;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      printf("[nvctl_test] PASS " __VA_ARGS__);                                \
      printf("\n");                                                            \
    } else {                                                                   \
      printf("[nvctl_test] FAIL " __VA_ARGS__);                                \
      printf(" (errno=%d)\n", errno);                                          \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

// _IOWR de Linux x86_64 a mano: musl no trae <linux/ioctl.h>.
#define NVIOC(nr, type) \
  ((3u << 30) | ((unsigned)sizeof(type) << 16) | ((unsigned)NV_IOCTL_MAGIC << 8) | (unsigned)(nr))

int main(void) {
  int ctl = open("/dev/nvidiactl", O_RDWR);
  CHECK(ctl >= 0, "open /dev/nvidiactl");
  if (ctl < 0) {
    printf("[nvctl_test] ALL FAIL\n");
    return 1;
  }

  nv_ioctl_rm_api_version_t ver;
  memset(&ver, 0, sizeof(ver));
  ver.cmd = NV_RM_API_VERSION_CMD_RELAXED;
  strcpy(ver.versionString, "615.78.08");
  int rc = ioctl(ctl, NVIOC(NV_ESC_CHECK_VERSION_STR, ver), &ver);
  CHECK(rc == 0 && ver.reply == NV_RM_API_VERSION_REPLY_RECOGNIZED,
        "CHECK_VERSION_STR rc=%d reply=%u", rc, ver.reply);

  nv_ioctl_card_info_t cards[NV_MAX_CARDS];
  memset(cards, 0, sizeof(cards));
  rc = ioctl(ctl, NVIOC(NV_ESC_CARD_INFO, cards), cards);
  int ncards = 0;
  for (int i = 0; i < NV_MAX_CARDS; i++) {
    if (!cards[i].valid) continue;
    ncards++;
    printf("[nvctl_test] card %d: %04x:%02x:%02x.%x id=%04x:%04x gpu_id=0x%x minor=%u irq=%u "
           "bar0=0x%llx/0x%llx bar1=0x%llx/0x%llx\n",
           i, cards[i].pci_info.domain, cards[i].pci_info.bus, cards[i].pci_info.slot,
           cards[i].pci_info.function, cards[i].pci_info.vendor_id, cards[i].pci_info.device_id,
           cards[i].gpu_id, cards[i].minor_number, cards[i].interrupt_line,
           (unsigned long long)cards[i].reg_address, (unsigned long long)cards[i].reg_size,
           (unsigned long long)cards[i].fb_address, (unsigned long long)cards[i].fb_size);
  }
  CHECK(rc == 0, "CARD_INFO rc=%d tarjetas=%d", rc, ncards);

  NVOS64_PARAMETERS al;
  memset(&al, 0, sizeof(al));
  al.hClass = NV01_ROOT_CLIENT;
  rc = ioctl(ctl, NVIOC(NV_ESC_RM_ALLOC, al), &al);
  CHECK(rc == 0 && al.status == 0, "RM_ALLOC NV01_ROOT_CLIENT rc=%d status=0x%x hClient=0x%x",
        rc, al.status, al.hObjectNew);
  NvHandle client = al.hObjectNew;

  if (rc == 0 && al.status == 0) {
    NV0000_CTRL_GPU_GET_ATTACHED_IDS_PARAMS ids;
    memset(&ids, 0xff, sizeof(ids));
    NVOS54_PARAMETERS ct;
    memset(&ct, 0, sizeof(ct));
    ct.hClient = client;
    ct.hObject = client;
    ct.cmd = NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS;
    ct.params = (NvP64)(uintptr_t)&ids;
    ct.paramsSize = sizeof(ids);
    rc = ioctl(ctl, NVIOC(NV_ESC_RM_CONTROL, ct), &ct);
    CHECK(rc == 0 && ct.status == 0, "RM_CONTROL GPU_GET_ATTACHED_IDS rc=%d status=0x%x", rc,
          ct.status);
    int n = 0;
    for (int i = 0; i < NV0000_CTRL_GPU_MAX_ATTACHED_GPUS; i++) {
      if (ids.gpuIds[i] == NV0000_CTRL_GPU_INVALID_ID) break;
      printf("[nvctl_test] GPU conectada: id=0x%x\n", ids.gpuIds[i]);
      n++;
    }
    printf("[nvctl_test] GPUs conectadas: %d\n", n);

    NVOS00_PARAMETERS fr;
    memset(&fr, 0, sizeof(fr));
    fr.hRoot = client;
    fr.hObjectParent = client;
    fr.hObjectOld = client;
    rc = ioctl(ctl, NVIOC(NV_ESC_RM_FREE, fr), &fr);
    CHECK(rc == 0 && fr.status == 0, "RM_FREE cliente rc=%d status=0x%x", rc, fr.status);
  }

  int g0 = open("/dev/nvidia0", O_RDWR);
  printf("[nvctl_test] open /dev/nvidia0: %s\n", g0 >= 0 ? "ok" : "no existe (sin GPU)");
  if (g0 >= 0) close(g0);
  close(ctl);
  printf("[nvctl_test] %s\n", g_fail ? "SOME FAIL" : "ALL PASS");
  return g_fail ? 1 : 0;
}
