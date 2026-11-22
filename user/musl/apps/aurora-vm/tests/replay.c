// tests/replay.c — reproduce una traza de QEMU (-trace memory_region_ops_*) contra
// los dispositivos de aurora-vm y compara cada lectura con lo que devolvio QEMU.
//
// Como se obtiene la traza (en un Linux con QEMU; sin KVM vale):
//   qemu-system-i386 -machine pc -m 512 -hda hd.img -cdrom reactos.iso -boot d \
//     -display none -serial file:serial.txt \
//     -trace 'memory_region_ops_*',file=trace.txt
// Compilar y usar:
//   cc -O2 -o replay tests/replay.c && ./replay trace.txt hd.img reactos.iso
//
// Se comparan IDE (registros, datos PIO de disco y CD), maestro de bus, PIC/ELCR,
// i8042, VGA (puertos, VBE y la ventana 0xA0000-0xBFFFF) y el puerto 0x92. El PIT,
// el RTC y el temporizador ACPI dependen del reloj y solo se alimentan. Las lecturas
// de estado de IDE en las que QEMU aun estaba ocupado (BSY: su E/S es asincrona) no
// se comparan: aqui todo termina en el acto.
#define _GNU_SOURCE
#include "../../../../../kernel/aurora_vm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_vm = -1;
static uint8_t *g_ram;
static uint64_t g_ram_size;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static void irq_set(int irq, int level);
static int irq_edge_pending(int irq) { (void)irq; return 0; }
static uint8_t *dma_ptr(uint64_t gpa, uint64_t len) {
  if (len > g_ram_size || gpa > g_ram_size - len)
    return NULL;
  return g_ram + gpa;
}

#include "../pic.h"
#include "../pit.h"
#include "../rtc.h"
#include "../ps2.h"
#include "../apic.h"
#include "../ide.h"
#include "../platform.h"
#include "../vga.h"

static int g_irq_level[24];
static void irq_set(int irq, int level) {
  if (irq < 16)
    pic_set_irq(irq, level);
  if (irq >= 0 && irq < 24)
    g_irq_level[irq] = level;
}

struct stat_dev {
  const char *name;
  int compare;
  unsigned long reads, writes, bad, skipped;
};
static struct stat_dev g_devs[] = {
    {"ide", 1, 0, 0, 0, 0},       {"piix-bmdma", 1, 0, 0, 0, 0}, {"bmdma", 1, 0, 0, 0, 0},
    {"pic", 1, 0, 0, 0, 0},       {"elcr", 1, 0, 0, 0, 0},       {"i8042-cmd", 1, 0, 0, 0, 0},
    {"i8042-data", 1, 0, 0, 0, 0}, {"vga", 1, 0, 0, 0, 0},       {"vbe", 1, 0, 0, 0, 0},
    {"vga-lowmem", 1, 0, 0, 0, 0}, {"port92", 1, 0, 0, 0, 0},    {"pit", 0, 0, 0, 0, 0},
    {"rtc", 0, 0, 0, 0, 0},       {"rtc-index", 0, 0, 0, 0, 0},  {"acpi-tmr", 0, 0, 0, 0, 0},
    {"pci-conf-idx", 0, 0, 0, 0, 0}, {"pci-conf-data", 0, 0, 0, 0, 0}, {"io", 0, 0, 0, 0, 0},
    {"acpi-evt", 0, 0, 0, 0, 0},  {"acpi-cnt", 0, 0, 0, 0, 0},   {"acpi-gpe0", 0, 0, 0, 0, 0},
};

static struct stat_dev *dev_by_name(const char *n) {
  for (size_t i = 0; i < sizeof(g_devs) / sizeof(g_devs[0]); i++)
    if (!strcmp(g_devs[i].name, n))
      return &g_devs[i];
  return NULL;
}

static int is_ide_status(uint64_t a) { return a == 0x1f7 || a == 0x177 || a == 0x3f6 || a == 0x376; }

// La traza no trae la memoria: al arrancar el maestro de bus se fabrica una tabla
// PRD justa para la transferencia en curso (como la del driver del huesped).
static unsigned long g_prd_made;
static void make_prd(struct ide_bus *b) {
  struct ide_if *s = &b->ifs[b->unit];
  uint32_t bytes;
  if (s->kind == IDE_CD)
    bytes = (uint32_t)s->packet_transfer_size;
  else
    bytes = (uint32_t)(s->nsector ? s->nsector : 256) * 512u;
  if (!bytes || b->bm_addr + 8ULL * 64 > g_ram_size)
    return;
  uint32_t buf = 0x01000000;
  uint8_t *prd = g_ram + b->bm_addr;
  for (int i = 0; bytes && i < 64; i++) {
    uint32_t c = bytes > 0x10000 ? 0x10000 : bytes;
    uint32_t len = c & 0xFFFF; // 0 = 64 KiB
    bytes -= c;
    uint32_t w1 = len | (bytes ? 0 : 0x80000000u);
    memcpy(prd + 8 * i, &buf, 4);
    memcpy(prd + 8 * i + 4, &w1, 4);
    buf += c;
  }
  g_prd_made++;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "uso: %s trace.txt hd.img cd.iso [max_errores]\n", argv[0]);
    return 2;
  }
  int max_err = argc > 4 ? atoi(argv[4]) : 40;
  g_ram_size = 512ULL << 20;
  g_ram = calloc(1, g_ram_size);
  g_plat = 1;
  g_acpi_on = 1;
  g_disk_fd = -1;
  pic_reset();
  pit_reset();
  cmos_init(512ULL << 20, 1);
  i8042_reset();
  pci_init();
  memset(g_ide, 0, sizeof(g_ide));
  // copia del disco: la VM lo escribe y no se quiere tocar el original
  char tmp[] = "/tmp/replay-hd-XXXXXX";
  int tfd = mkstemp(tmp);
  {
    int sfd = open(argv[2], O_RDONLY);
    struct stat st;
    fstat(sfd, &st);
    if (ftruncate(tfd, st.st_size) < 0)
      perror("ftruncate");
    static uint8_t buf[1 << 20];
    for (off_t o = 0; o < st.st_size;) {
      ssize_t n = pread(sfd, buf, sizeof(buf), o);
      if (n <= 0)
        break;
      int zero = 1;
      for (ssize_t i = 0; i < n && zero; i++)
        zero = buf[i] == 0;
      if (!zero && pwrite(tfd, buf, (size_t)n, o) != n)
        perror("pwrite");
      o += n;
    }
    close(sfd);
    close(tfd);
  }
  if (ide_attach(0, 0, IDE_HD, tmp) < 0 || ide_attach(1, 0, IDE_CD, argv[3]) < 0)
    return 1;
  ide_init();
  ide_reset_all();
  static uint8_t rom[512];
  if (vga_setup(rom, sizeof(rom)) < 0) {
    perror("vga_setup");
    return 1;
  }
  FILE *f = fopen(argv[1], "r");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  char line[512];
  unsigned long lineno = 0, nerr = 0, unknown = 0;
  // ultimas lineas (contexto de los errores)
  static char ctx[8][512];
  while (fgets(line, sizeof(line), f)) {
    lineno++;
    snprintf(ctx[lineno % 8], sizeof(ctx[0]), "%s", line);
    int is_read;
    if (!strncmp(line, "memory_region_ops_read ", 23))
      is_read = 1;
    else if (!strncmp(line, "memory_region_ops_write ", 24))
      is_read = 0;
    else
      continue;
    char *pa = strstr(line, " addr "), *pv = strstr(line, " value "), *ps = strstr(line, " size "),
         *pn = strstr(line, " name '");
    if (!pa || !pv || !ps || !pn)
      continue;
    uint64_t addr = strtoull(pa + 6, NULL, 16), val = strtoull(pv + 7, NULL, 16);
    int size = atoi(ps + 6);
    char name[64];
    snprintf(name, sizeof(name), "%s", pn + 7);
    char *q = strchr(name, '\'');
    if (q)
      *q = 0;
    struct stat_dev *d = dev_by_name(name);
    if (!d) {
      unknown++;
      continue;
    }
    if (!strcmp(name, "vga-lowmem")) {
      uint8_t b[8] = {0};
      if (is_read) {
        vga_mmio_read(addr, b, (unsigned)size);
        uint64_t got = 0;
        memcpy(&got, b, (size_t)size);
        d->reads++;
        if (got != val) {
          d->bad++;
          if (nerr++ < (unsigned long)max_err)
            printf("L%lu %s lectura 0x%llx/%d: QEMU 0x%llx, aqui 0x%llx\n", lineno, name,
                   (unsigned long long)addr, size, (unsigned long long)val, (unsigned long long)got);
        }
      } else {
        memcpy(b, &val, (size_t)size);
        vga_mmio_write(addr, b, (unsigned)size);
        d->writes++;
      }
      continue;
    }
    uint16_t port = (uint16_t)addr;
    if (is_read) {
      uint32_t got = 0xFFFFFFFFu;
      if (!plat_in(port, size, &got)) {
        d->skipped++;
        continue;
      }
      if (size == 1)
        got &= 0xFF;
      else if (size == 2)
        got &= 0xFFFF;
      d->reads++;
      if (!d->compare)
        continue;
      if (!strcmp(name, "piix-bmdma") && (port & 7) == 2 && (val & 5) == 1) {
        d->skipped++; // DMA de QEMU aun en curso
        continue;
      }
      if (!strcmp(name, "ide") && is_ide_status(addr) && (val & 0x80)) {
        d->skipped++; // QEMU aun ocupado
        continue;
      }
      if (got != (uint32_t)val) {
        d->bad++;
        if (nerr++ < (unsigned long)max_err) {
          printf("L%lu %s lectura 0x%x/%d: QEMU 0x%llx, aqui 0x%x\n", lineno, name, port, size,
                 (unsigned long long)val, got);
          for (int k = 7; k >= 1; k--)
            if (lineno > (unsigned long)k)
              printf("     | %s", ctx[(lineno - (unsigned long)k) % 8]);
        }
      }
    } else {
      if (!strcmp(name, "piix-bmdma") && (port & 7) == 0 && (val & 1)) {
        struct ide_bus *b = &g_ide[(port >> 3) & 1];
        if (!(b->bm_cmd & 1))
          make_prd(b);
      }
      if (!plat_out(port, (uint32_t)val, size))
        d->skipped++;
      d->writes++;
    }
  }
  fclose(f);
  unlink(tmp);
  printf("\n%-14s %10s %10s %8s %8s\n", "dispositivo", "lecturas", "escrituras", "malas", "omitidas");
  unsigned long tbad = 0;
  for (size_t i = 0; i < sizeof(g_devs) / sizeof(g_devs[0]); i++) {
    struct stat_dev *d = &g_devs[i];
    if (!d->reads && !d->writes)
      continue;
    printf("%-14s %10lu %10lu %8lu %8lu%s\n", d->name, d->reads, d->writes, d->bad, d->skipped,
           d->compare ? "" : "  (no se compara)");
    tbad += d->bad;
  }
  printf("lineas de otros dispositivos: %lu; tablas PRD fabricadas: %lu\n", unknown, g_prd_made);
  printf("%s: %lu lecturas distintas\n", tbad ? "FALLA" : "OK", tbad);
  return tbad ? 1 : 0;
}
