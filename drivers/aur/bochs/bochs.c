// drivers/aur/bochs/bochs.c
//
// [FASE 6.2] bochs.aur — adaptador VGA de QEMU/Bochs (1234:1111, "-vga std").
//
// Con UEFI el framebuffer lineal ya es visible (GOP): el driver no presenta
// nada. Lo que aporta es la descripcion del adaptador para DRM, leida del
// propio dispositivo por MMIO (BAR2):
//   - EDID de 128 bytes en BAR2+0x000 (QEMU lo sirve ahi),
//   - registros VBE "dispi" en BAR2+0x500 (ID, X/Y maximo) -> modos validos.
// Un cambio de resolucion en caliente queda fuera: el compositor del kernel
// trabaja a la resolucion de arranque.

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GPUP_INFO 0x800
#define GPUP_REGISTER 0x801
#define AUR_GPU_MAX_MODES 16
struct aur_gpu_info {
  uint64_t fb_phys, fb_size;
  uint32_t width, height, pitch, bpp;
};
struct aur_gpu_mode {
  uint16_t width, height, refresh, flags;
};
struct aur_gpu_reg {
  char name[32];
  uint32_t flags, n_modes;
  struct aur_gpu_mode modes[AUR_GPU_MAX_MODES];
  uint32_t edid_len, _pad;
  uint8_t edid[128];
};
_Static_assert(sizeof(struct aur_gpu_info) == 32, "ABI aur_gpu_info");
_Static_assert(sizeof(struct aur_gpu_reg) == 304, "ABI aur_gpu_reg");

#define PCI_SYSFS "/sys/bus/pci/devices"
#define DISPI_BASE 0x500
#define DISPI_INDEX_ID 0
#define DISPI_INDEX_XRES 1
#define DISPI_INDEX_YRES 2
#define DISPI_INDEX_VIRT_WIDTH 6

static unsigned long read_sysfs_hex(const char *bdf, const char *attr) {
  char path[512], buf[32];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/%s", bdf, attr);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return (unsigned long)-1;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return (unsigned long)-1;
  buf[n] = 0;
  return strtoul(buf, NULL, 16);
}

static int find_device(char *out, size_t len) {
  DIR *d = opendir(PCI_SYSFS);
  if (!d)
    return -1;
  struct dirent *e;
  int rc = -1;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' || strlen(e->d_name) >= len)
      continue;
    if (read_sysfs_hex(e->d_name, "vendor") == 0x1234 &&
        read_sysfs_hex(e->d_name, "device") == 0x1111) {
      strcpy(out, e->d_name);
      rc = 0;
      break;
    }
  }
  closedir(d);
  return rc;
}

static int edid_valid(const uint8_t *e) {
  static const uint8_t magic[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
  if (memcmp(e, magic, 8) != 0)
    return 0;
  uint8_t sum = 0;
  for (int i = 0; i < 128; i++)
    sum += e[i];
  return sum == 0;
}

int main(int argc, char **argv) {
  printf("[bochs.aur] Iniciando driver en Ring 3...\n");
  char bdf[64];
  if (argc > 1 && strlen(argv[1]) < sizeof(bdf))
    strcpy(bdf, argv[1]);
  else if (find_device(bdf, sizeof(bdf)) != 0) {
    fprintf(stderr, "[bochs.aur] no hay 1234:1111 en sysfs\n");
    return 1;
  }
  int fd_gpu = open("/dev/gpuproxy", O_RDWR);
  if (fd_gpu < 0) {
    perror("[bochs.aur] open /dev/gpuproxy");
    return 1;
  }
  struct aur_gpu_info info;
  // El framebuffer lo publica el kernel al iniciar la pantalla: este driver
  // puede arrancar antes, asi que se reintenta unos segundos.
  int have_info = 0;
  for (int i = 0; i < 150 && !have_info; i++) {
    if (ioctl(fd_gpu, GPUP_INFO, &info) == 0)
      have_info = 1;
    else
      usleep(100000);
  }
  if (!have_info) {
    perror("[bochs.aur] GPUP_INFO");
    return 1;
  }

  struct aur_gpu_reg reg;
  memset(&reg, 0, sizeof(reg));
  snprintf(reg.name, sizeof(reg.name), "bochs-dispi");
  reg.n_modes = 1;
  reg.modes[0].width = (uint16_t)info.width;
  reg.modes[0].height = (uint16_t)info.height;
  reg.modes[0].refresh = 60;
  reg.modes[0].flags = 1;

  char path[512];
  snprintf(path, sizeof(path), PCI_SYSFS "/%s/resource2", bdf);
  int fd = open(path, O_RDWR | O_SYNC);
  volatile uint8_t *mmio =
      fd >= 0 ? mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
              : MAP_FAILED;
  if (mmio != MAP_FAILED) {
    uint8_t edid[128];
    for (int i = 0; i < 128; i++)
      edid[i] = mmio[i];
    if (edid_valid(edid)) {
      memcpy(reg.edid, edid, 128);
      reg.edid_len = 128;
    }
    volatile uint16_t *dispi = (volatile uint16_t *)(mmio + DISPI_BASE);
    printf("[bochs.aur] DISPI id=0x%04x res actual=%ux%u\n",
           dispi[DISPI_INDEX_ID], dispi[DISPI_INDEX_XRES],
           dispi[DISPI_INDEX_YRES]);
  } else {
    fprintf(stderr, "[bochs.aur] sin BAR2 (EDID no disponible)\n");
  }
  if (ioctl(fd_gpu, GPUP_REGISTER, &reg) != 0) {
    perror("[bochs.aur] GPUP_REGISTER");
    return 1;
  }
  printf("[bochs.aur] adaptador registrado (edid=%u)\n", reg.edid_len);
  fflush(stdout);
  // El registro vive mientras el fd este abierto: quedarse dormido.
  for (;;)
    sleep(3600);
}
