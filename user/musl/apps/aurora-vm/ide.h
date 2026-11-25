// ide.h — IDE PIIX3 (dos canales en modo heredado 0x1F0/0x3F6 IRQ14 y 0x170/0x376
// IRQ15) con discos ATA y CD-ROM ATAPI, y DMA de bus maestro (BAR4). Es un porte
// directo y sincrono de QEMU (hw/ide/core.c, hw/ide/atapi.c, hw/ide/pci.c,
// hw/block/cdrom.c): mismos registros, firmas, IDENTIFY, maquina de estados PIO
// (end_transfer_func), lectura/escritura multiple y LBA48, DMA por tabla PRD con
// el bit de interrupcion del maestro de bus, y los comandos ATAPI que usan
// Windows y ReactOS (INQUIRY, READ(10/12), READ CD, TOC, GET CONFIGURATION,
// GET EVENT STATUS, MODE SENSE, REQUEST SENSE...). Las E/S al fichero son
// sincronas: donde QEMU pone BSY y espera a la AIO, aqui la operacion ya acabo.
#ifndef AVM_IDE_H
#define AVM_IDE_H

static void irq_set(int irq, int level);
static uint8_t *dma_ptr(uint64_t gpa, uint64_t len); // NULL si no es RAM

#define IDE_NONE 0
#define IDE_HD 1
#define IDE_CD 2

#define ERR_STAT 0x01
#define DRQ_STAT 0x08
#define SEEK_STAT 0x10
#define READY_STAT 0x40
#define BUSY_STAT 0x80
#define ABRT_ERR 0x04
#define MC_ERR 0x20
#define ATA_DEV_SELECT 0x10
#define ATA_DEV_ALWAYS_ON 0xA0
#define ATA_DEV_LBA 0x40
#define ATA_DEV_HS 0x0F
#define IDE_CTRL_HOB 0x80
#define IDE_CTRL_RESET 0x04
#define IDE_CTRL_DISABLE_IRQ 0x02
#define ATAPI_INT_REASON_CD 0x01
#define ATAPI_INT_REASON_IO 0x02
#define MAX_MULT_SECTORS 16
#define IDE_DMA_BUF_SECTORS 256
#define IDE_IOBUF (IDE_DMA_BUF_SECTORS * 512 + 4)
#define ATAPI_PACKET_SIZE 12
#define CD_MAX_SECTORS (80 * 60 * 75 * 4)
#define BM_STATUS_DMAING 0x01
#define BM_STATUS_ERROR 0x02
#define BM_STATUS_INT 0x04
#define BM_CMD_START 0x01
#define BMDMA_PAGE_SIZE 4096

#define SENSE_NONE 0
#define SENSE_NOT_READY 2
#define SENSE_ILLEGAL_REQUEST 5
#define SENSE_UNIT_ATTENTION 6
#define ASC_ILLEGAL_OPCODE 0x20
#define ASC_LOGICAL_BLOCK_OOR 0x21
#define ASC_INV_FIELD_IN_CMD_PACKET 0x24
#define ASC_MEDIUM_MAY_HAVE_CHANGED 0x28
#define ASC_INCOMPATIBLE_FORMAT 0x30
#define ASC_SAVING_PARAMETERS_NOT_SUPPORTED 0x39
#define ASC_MEDIUM_NOT_PRESENT 0x3a
#define ASC_DATA_PHASE_ERROR 0x4b
#define ASC_MEDIA_REMOVAL_PREVENTED 0x53

enum { ET_DUMMY, ET_STOP, ET_SECTOR_READ, ET_SECTOR_WRITE, ET_ATAPI_CMD, ET_ATAPI_REPLY };
enum { DMA_NONE, DMA_READ, DMA_WRITE, DMA_ATAPI };
enum { CB_NONE, CB_ATA, CB_ATAPI };

struct ide_bus;
struct ide_if {
  struct ide_bus *bus;
  int unit, kind, fd, ro;
  uint64_t nb_sectors; // de 512 bytes
  int cylinders, heads, sectors, drive_heads, drive_sectors, reset_reverts;
  int feature, error, nsector, sector, lcyl, hcyl;
  int hob_feature, hob_nsector, hob_sector, hob_lcyl, hob_hcyl;
  int select, status, lba48, mult_sectors, req_nb_sectors;
  uint16_t identify[256];
  int identify_set, wcache;
  uint8_t io_buffer[IDE_IOBUF];
  int data_ptr, data_end, end_transfer;
  int io_buffer_index, io_buffer_size, dma_cmd;
  // ATAPI
  int sense_key, asc, packet_transfer_size, elementary_transfer_size, lba;
  int cd_sector_size, atapi_dma, tray_locked, tray_open, cdrom_changed, new_media;
  char serial[21], model[41];
};
struct ide_bus {
  struct ide_if ifs[2];
  int unit, cmd, irq, irq_level;
  // maestro de bus
  uint8_t bm_cmd, bm_status;
  uint32_t bm_addr, cur_addr, cur_prd_addr, cur_prd_len;
  int cur_prd_last, dma_cb;
};
static struct ide_bus g_ide[2];

// ---------------------------------------------------------------------------
// Comprobacion de integridad del disco (AURORA_VM_TRAZA o AURORA_VM_DISCO_CHECK):
// se guarda un resumen de cada sector de 512 bytes escrito o leido (tabla de
// acceso directo de 4M entradas, 32 MiB; las viejas se pisan) y cada lectura se
// compara con lo ultimo visto de ese sector. Si el sistema de ficheros del
// anfitrion devolviera algo distinto de lo que se escribio (o la ISO, que es de
// solo lectura, cambiara entre dos lecturas), aqui se ve.
// ---------------------------------------------------------------------------
#define DCHK_BITS 22
static uint64_t *g_dchk;
static int g_dchk_on;
static uint64_t g_dchk_bad, g_dchk_sect;
static uint8_t g_dchk_stage[512];
static unsigned g_dchk_fill;
static uint64_t g_dchk_cur; // clave del sector que se esta juntando

static uint32_t dchk_hash(const uint8_t *p) {
  uint64_t h = 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 512; i += 8) {
    uint64_t w;
    memcpy(&w, p + i, 8);
    h = (h ^ w) * 0xFF51AFD7ED558CCDULL;
    h ^= h >> 29;
  }
  return (uint32_t)(h >> 40) & 0xFFFFFF;
}

static int dchk_dev(const struct ide_if *s) { return (int)(s->bus - g_ide) * 2 + s->unit; }

static void dchk_sector(uint64_t key, const uint8_t *p, int write) {
  g_dchk_sect++;
  uint32_t h = dchk_hash(p);
  uint64_t idx = (key * 0x9E3779B97F4A7C15ULL) >> (64 - DCHK_BITS);
  uint64_t e = g_dchk[idx], tag = (key + 1) << 24;
  if (!write && (e & ~0xFFFFFFULL) == tag && (uint32_t)(e & 0xFFFFFF) != h) {
    g_dchk_bad++;
    if (g_dchk_bad <= 20) {
      int zero = 1;
      for (int i = 0; i < 512 && zero; i++)
        zero = p[i] == 0;
      fprintf(stderr, "[disco] FALLO DE INTEGRIDAD: unidad %d sector %llu: lo leido no es lo "
                      "ultimo escrito/leido (resumen %06x, esperado %06x)%s\n",
              (int)(key >> 39), (unsigned long long)(key & ((1ULL << 39) - 1)), h,
              (unsigned)(e & 0xFFFFFF), zero ? " [leido a ceros]" : "");
    }
  }
  g_dchk[idx] = tag | h;
}

// Pasa len bytes contiguos del disco (desde el byte off, multiplo de 512).
static void dchk_begin(const struct ide_if *s, uint64_t off) {
  g_dchk_cur = ((uint64_t)dchk_dev(s) << 39) | (off >> 9);
  g_dchk_fill = 0;
}
static void dchk_feed(const uint8_t *p, uint64_t len, int write) {
  while (len) {
    if (!g_dchk_fill && len >= 512) {
      dchk_sector(g_dchk_cur++, p, write);
      p += 512;
      len -= 512;
      continue;
    }
    unsigned c = 512 - g_dchk_fill;
    if (c > len)
      c = (unsigned)len;
    memcpy(g_dchk_stage + g_dchk_fill, p, c);
    g_dchk_fill += c;
    p += c;
    len -= c;
    if (g_dchk_fill == 512) {
      dchk_sector(g_dchk_cur++, g_dchk_stage, write);
      g_dchk_fill = 0;
    }
  }
}
static void dchk_buf(const struct ide_if *s, uint64_t off, const uint8_t *p, uint64_t len, int write) {
  if (!g_dchk)
    return;
  dchk_begin(s, off);
  dchk_feed(p, len, write);
}
static void dchk_init(void) {
  if (!g_dchk_on || g_dchk)
    return;
  g_dchk = calloc((size_t)1 << DCHK_BITS, sizeof(uint64_t));
  if (!g_dchk)
    fprintf(stderr, "aurora-vm: sin memoria para comprobar el disco\n");
}
static int g_ide_on;

static void put_be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static unsigned get_be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t get_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static struct ide_if *ide_active(struct ide_bus *b) { return &b->ifs[b->unit]; }

static void ide_bus_set_irq(struct ide_bus *b) {
  if (b->cmd & IDE_CTRL_DISABLE_IRQ)
    return;
  b->bm_status |= BM_STATUS_INT; // bmdma_irq de QEMU
  if (!b->irq_level) {
    b->irq_level = 1;
    irq_set(b->irq, 1);
  }
}
static void ide_bus_lower_irq(struct ide_bus *b) {
  if (b->irq_level) {
    b->irq_level = 0;
    irq_set(b->irq, 0);
  }
}

static void padstr(uint16_t *w, const char *src, int len) { // len en bytes, intercambiados
  char *str = (char *)w;
  for (int i = 0; i < len; i++) {
    int v = *src ? *src++ : ' ';
    str[i ^ 1] = (char)v;
  }
}
static void padstr8(uint8_t *buf, int n, const char *src) {
  for (int i = 0; i < n; i++)
    buf[i] = (uint8_t)(*src ? *src++ : ' ');
}

// ---------------------------------------------------------------------------
// IDENTIFY
// ---------------------------------------------------------------------------
static void ide_identify(struct ide_if *s) {
  uint16_t *p = s->identify;
  if (!s->identify_set) {
    memset(p, 0, 512);
    p[0] = 0x0040;
    p[1] = (uint16_t)s->cylinders;
    p[3] = (uint16_t)s->heads;
    p[4] = (uint16_t)(512 * s->sectors);
    p[5] = 512;
    p[6] = (uint16_t)s->sectors;
    padstr(p + 10, s->serial, 20);
    p[20] = 3;
    p[21] = 512;
    p[22] = 4;
    padstr(p + 23, "2.5+", 8);
    padstr(p + 27, s->model, 40);
    p[47] = 0x8000 | MAX_MULT_SECTORS;
    p[48] = 1;
    p[49] = (1 << 11) | (1 << 9) | (1 << 8);
    p[51] = 0x200;
    p[52] = 0x200;
    p[53] = 1 | 2 | 4;
    p[54] = (uint16_t)s->cylinders;
    p[55] = (uint16_t)s->heads;
    p[56] = (uint16_t)s->sectors;
    unsigned old = (unsigned)(s->cylinders * s->heads * s->sectors);
    p[57] = (uint16_t)old;
    p[58] = (uint16_t)(old >> 16);
    if (s->mult_sectors)
      p[59] = (uint16_t)(0x100 | s->mult_sectors);
    p[62] = 0x07;
    p[63] = 0x07;
    p[64] = 0x03;
    p[65] = p[66] = p[67] = p[68] = 120;
    p[80] = 0xf0;
    p[81] = 0x16;
    p[82] = (1 << 14) | (1 << 5); // sin SMART (no se emula)
    p[83] = (1 << 14) | (1 << 13) | (1 << 12) | (1 << 10);
    p[84] = 1 << 14;
    p[85] = s->wcache ? ((1 << 14) | (1 << 5)) : (1 << 14);
    p[86] = (1 << 13) | (1 << 12) | (1 << 10);
    p[87] = 1 << 14;
    p[88] = 0x3f | (1 << 13);
    p[93] = 1 | (1 << 14) | 0x2000;
    uint64_t l28 = s->nb_sectors >= (1u << 28) ? (1u << 28) - 1 : s->nb_sectors;
    p[60] = (uint16_t)l28;
    p[61] = (uint16_t)(l28 >> 16);
    p[100] = (uint16_t)s->nb_sectors;
    p[101] = (uint16_t)(s->nb_sectors >> 16);
    p[102] = (uint16_t)(s->nb_sectors >> 32);
    p[103] = (uint16_t)(s->nb_sectors >> 48);
    p[106] = 0x6000; // un sector logico por fisico (512 B), como QEMU
    s->identify_set = 1;
  }
  memcpy(s->io_buffer, p, 512);
}

static void ide_atapi_identify(struct ide_if *s) {
  uint16_t *p = s->identify;
  if (!s->identify_set) {
    memset(p, 0, 512);
    p[0] = (2 << 14) | (5 << 8) | (1 << 7) | (2 << 5);
    padstr(p + 10, s->serial, 20);
    p[20] = 3;
    p[21] = 512;
    p[22] = 4;
    padstr(p + 23, "2.5+", 8);
    padstr(p + 27, s->model, 40);
    p[48] = 1;
    p[49] = 1 << 9 | 1 << 8;
    p[53] = 7;
    p[62] = 7;
    p[63] = 7;
    p[64] = 3;
    p[65] = 0xb4;
    p[66] = 0xb4;
    p[67] = 0x12c;
    p[68] = 0xb4;
    p[71] = 30;
    p[72] = 30;
    p[80] = 0x1e;
    p[88] = 0x3f | (1 << 13);
    s->identify_set = 1;
  }
  memcpy(s->io_buffer, p, 512);
}

static void ide_set_signature(struct ide_if *s) {
  s->select &= ~ATA_DEV_HS;
  s->nsector = 1;
  s->sector = 1;
  if (s->kind == IDE_CD) {
    s->lcyl = 0x14;
    s->hcyl = 0xeb;
  } else if (s->kind == IDE_HD) {
    s->lcyl = 0;
    s->hcyl = 0;
  } else {
    s->lcyl = 0xff;
    s->hcyl = 0xff;
  }
}

// ---------------------------------------------------------------------------
// Transferencias PIO
// ---------------------------------------------------------------------------
static void ide_transfer_start(struct ide_if *s, int off, int size, int et) {
  s->data_ptr = off;
  s->data_end = off + size;
  if (!(s->status & ERR_STAT))
    s->status |= DRQ_STAT;
  s->end_transfer = et;
}
static void ide_transfer_halt(struct ide_if *s) {
  s->end_transfer = ET_STOP;
  s->data_ptr = s->data_end = 0;
  s->status &= ~DRQ_STAT;
}
static void ide_transfer_stop(struct ide_if *s) { ide_transfer_halt(s); }

static void ide_dummy_transfer_stop(struct ide_if *s) {
  s->data_ptr = s->data_end = 0;
  s->io_buffer[0] = s->io_buffer[1] = s->io_buffer[2] = s->io_buffer[3] = 0xff;
}

static void ide_abort_command(struct ide_if *s) {
  s->status = READY_STAT | ERR_STAT;
  s->error = ABRT_ERR;
  ide_transfer_stop(s);
}

static int64_t ide_get_sector(struct ide_if *s) {
  if (s->select & ATA_DEV_LBA) {
    if (s->lba48)
      return ((int64_t)s->hob_hcyl << 40) | ((int64_t)s->hob_lcyl << 32) |
             ((int64_t)s->hob_sector << 24) | ((int64_t)s->hcyl << 16) |
             ((int64_t)s->lcyl << 8) | s->sector;
    return ((int64_t)(s->select & 0x0F) << 24) | (s->hcyl << 16) | (s->lcyl << 8) | s->sector;
  }
  return (int64_t)((s->hcyl << 8) | s->lcyl) * s->heads * s->sectors +
         (s->select & ATA_DEV_HS) * s->sectors + (s->sector - 1);
}

static void ide_set_sector(struct ide_if *s, int64_t n) {
  if (s->select & ATA_DEV_LBA) {
    if (s->lba48) {
      s->sector = (uint8_t)n;
      s->lcyl = (uint8_t)(n >> 8);
      s->hcyl = (uint8_t)(n >> 16);
      s->hob_sector = (uint8_t)(n >> 24);
      s->hob_lcyl = (uint8_t)(n >> 32);
      s->hob_hcyl = (uint8_t)(n >> 40);
    } else {
      s->select = (s->select & ~0x0F) | (int)((n >> 24) & 0x0F);
      s->hcyl = (uint8_t)(n >> 16);
      s->lcyl = (uint8_t)(n >> 8);
      s->sector = (uint8_t)n;
    }
  } else {
    unsigned hs = (unsigned)(s->heads * s->sectors);
    if (!hs)
      return;
    unsigned cyl = (unsigned)(n / hs), r = (unsigned)(n % hs);
    s->hcyl = (uint8_t)(cyl >> 8);
    s->lcyl = (uint8_t)cyl;
    s->select = (s->select & ~ATA_DEV_HS) | (int)((r / (unsigned)s->sectors) & ATA_DEV_HS);
    s->sector = (int)(r % (unsigned)s->sectors) + 1;
  }
}

static int ide_sect_range_ok(struct ide_if *s, uint64_t sector, uint64_t n) {
  return !(sector > s->nb_sectors || n > s->nb_sectors - sector);
}

static void ide_rw_error(struct ide_if *s) {
  ide_abort_command(s);
  ide_bus_set_irq(s->bus);
}

static void ide_sector_read(struct ide_if *s) {
  s->status = READY_STAT | SEEK_STAT;
  s->error = 0;
  int64_t sn = ide_get_sector(s);
  int n = s->nsector;
  if (n == 0) {
    ide_transfer_stop(s);
    return;
  }
  if (n > s->req_nb_sectors)
    n = s->req_nb_sectors;
  if (!ide_sect_range_ok(s, (uint64_t)sn, (uint64_t)n) ||
      vm_pread(s->fd, s->io_buffer, (size_t)n * 512, (off_t)sn * 512) != (ssize_t)n * 512) {
    ide_rw_error(s);
    return;
  }
  dchk_buf(s, (uint64_t)sn * 512, s->io_buffer, (uint64_t)n * 512, 0);
  // ide_sector_read_cb
  ide_set_sector(s, sn + n);
  s->nsector -= n;
  ide_transfer_start(s, 0, n * 512, ET_SECTOR_READ);
  ide_bus_set_irq(s->bus);
}

static void ide_sector_write(struct ide_if *s) {
  s->status = READY_STAT | SEEK_STAT | BUSY_STAT;
  int64_t sn = ide_get_sector(s);
  int n = s->nsector;
  if (n > s->req_nb_sectors)
    n = s->req_nb_sectors;
  if (!ide_sect_range_ok(s, (uint64_t)sn, (uint64_t)n) || s->ro ||
      vm_pwrite(s->fd, s->io_buffer, (size_t)n * 512, (off_t)sn * 512) != (ssize_t)n * 512) {
    ide_rw_error(s);
    return;
  }
  dchk_buf(s, (uint64_t)sn * 512, s->io_buffer, (uint64_t)n * 512, 1);
  // ide_sector_write_cb
  s->status &= ~BUSY_STAT;
  s->nsector -= n;
  ide_set_sector(s, sn + n);
  if (s->nsector == 0) {
    ide_transfer_stop(s);
  } else {
    int n1 = s->nsector < s->req_nb_sectors ? s->nsector : s->req_nb_sectors;
    ide_transfer_start(s, 0, n1 * 512, ET_SECTOR_WRITE);
  }
  ide_bus_set_irq(s->bus);
}

static void ide_atapi_cmd(struct ide_if *s);
static void ide_atapi_cmd_reply_end(struct ide_if *s);

static void ide_end_transfer(struct ide_if *s) {
  switch (s->end_transfer) {
  case ET_STOP: ide_transfer_stop(s); break;
  case ET_SECTOR_READ: ide_sector_read(s); break;
  case ET_SECTOR_WRITE: ide_sector_write(s); break;
  case ET_ATAPI_CMD: ide_atapi_cmd(s); break;
  case ET_ATAPI_REPLY: ide_atapi_cmd_reply_end(s); break;
  default: ide_dummy_transfer_stop(s); break;
  }
}

static int ide_is_pio_out(struct ide_if *s) {
  return !(s->end_transfer == ET_SECTOR_WRITE || s->end_transfer == ET_ATAPI_CMD);
}

// ---------------------------------------------------------------------------
// DMA de bus maestro (hw/ide/pci.c)
// ---------------------------------------------------------------------------
struct sgent { uint64_t addr; uint32_t len; };
static struct sgent g_sg[8192];
static int g_sg_n;
static uint32_t g_sg_size;

static int bm_read_prd(struct ide_bus *b) {
  if (b->cur_prd_last || (b->cur_addr - b->bm_addr) >= BMDMA_PAGE_SIZE)
    return 0;
  uint8_t *p = dma_ptr(b->cur_addr, 8);
  uint32_t addr = 0, size = 0;
  if (p) {
    memcpy(&addr, p, 4);
    memcpy(&size, p + 4, 4);
  } else {
    addr = size = 0xFFFFFFFFu; // lectura fuera de la RAM: todo unos
  }
  b->cur_addr += 8;
  uint32_t len = size & 0xfffe;
  if (!len)
    len = 0x10000;
  b->cur_prd_len = len;
  b->cur_prd_addr = addr;
  b->cur_prd_last = (size & 0x80000000u) != 0;
  return 1;
}

// bmdma_prepare_buf: lista de segmentos (hasta limit bytes); consume toda la tabla.
static int32_t bm_prepare_buf(struct ide_bus *b, struct ide_if *s, int32_t limit) {
  g_sg_n = 0;
  g_sg_size = 0;
  s->io_buffer_size = 0;
  for (;;) {
    if (b->cur_prd_len == 0 && !bm_read_prd(b))
      return (int32_t)g_sg_size;
    uint32_t l = b->cur_prd_len;
    if (l > 0) {
      uint32_t sg_len = (uint32_t)limit - g_sg_size;
      if (sg_len > b->cur_prd_len)
        sg_len = b->cur_prd_len;
      if (sg_len && g_sg_n < (int)(sizeof(g_sg) / sizeof(g_sg[0]))) {
        g_sg[g_sg_n].addr = b->cur_prd_addr;
        g_sg[g_sg_n].len = sg_len;
        g_sg_n++;
        g_sg_size += sg_len;
      }
      b->cur_prd_addr += l;
      b->cur_prd_len -= l;
      s->io_buffer_size += (int)l;
    }
  }
}

// bmdma_rw_buf: copia io_buffer[index..size) a/desde la tabla PRD. 0 = tabla agotada.
static int bm_rw_buf(struct ide_bus *b, struct ide_if *s, int to_guest) {
  for (;;) {
    int l = s->io_buffer_size - s->io_buffer_index;
    if (l <= 0)
      break;
    if (b->cur_prd_len == 0 && !bm_read_prd(b))
      return 0;
    if (l > (int)b->cur_prd_len)
      l = (int)b->cur_prd_len;
    if (l > 0) {
      uint8_t *g = dma_ptr(b->cur_prd_addr, (uint64_t)l);
      if (g) {
        if (to_guest)
          memcpy(g, s->io_buffer + s->io_buffer_index, (size_t)l);
        else
          memcpy(s->io_buffer + s->io_buffer_index, g, (size_t)l);
      }
      b->cur_prd_addr += (uint32_t)l;
      b->cur_prd_len -= (uint32_t)l;
      s->io_buffer_index += l;
    }
  }
  return 1;
}

static void ide_set_inactive(struct ide_if *s, int more) {
  struct ide_bus *b = s->bus;
  b->bm_status &= ~BM_STATUS_DMAING;
  if (more)
    b->bm_status |= BM_STATUS_DMAING;
  b->dma_cb = CB_NONE;
}

static void ide_dma_error(struct ide_if *s) {
  ide_abort_command(s);
  ide_set_inactive(s, 0);
  ide_bus_set_irq(s->bus);
}

// Transfiere la lista de segmentos entre el disco y la RAM del huesped.
static int ide_sg_io(struct ide_if *s, uint64_t off, int write) {
  // El resumen de integridad se toma justo despues de cada segmento: Windows
  // apunta varios segmentos de una lectura a la misma pagina (su "pagina
  // ficticia" para lo que no necesita) y, resumiendo al final, todos salian con
  // el contenido del ultimo -> falsos FALLO DE INTEGRIDAD (87.000 en vm33).
  if (g_dchk)
    dchk_begin(s, off);
  for (int i = 0; i < g_sg_n; i++) {
    uint8_t *g = dma_ptr(g_sg[i].addr, g_sg[i].len);
    if (!g)
      return -1;
    ssize_t r = write ? vm_pwrite(s->fd, g, g_sg[i].len, (off_t)off) : vm_pread(s->fd, g, g_sg[i].len, (off_t)off);
    if (r != (ssize_t)g_sg[i].len)
      return -1;
    if (g_dchk)
      dchk_feed(g, g_sg[i].len, write);
    off += g_sg[i].len;
  }
  return 0;
}

// ide_dma_cb, en bucle: cada vuelta completa la E/S anterior y lanza la siguiente.
static void ide_dma_cb(struct ide_if *s) {
  int ret = 0;
  for (;;) {
    int stay_active = 0, n;
    if (ret < 0) {
      ide_dma_error(s);
      return;
    }
    if (s->io_buffer_size > s->nsector * 512) {
      n = s->nsector;
      stay_active = 1;
    } else {
      n = s->io_buffer_size >> 9;
    }
    int64_t sn = ide_get_sector(s);
    if (n > 0) {
      sn += n;
      ide_set_sector(s, sn);
      s->nsector -= n;
    }
    if (s->nsector == 0) {
      s->status = READY_STAT | SEEK_STAT;
      ide_bus_set_irq(s->bus);
      ide_set_inactive(s, stay_active);
      return;
    }
    n = s->nsector;
    s->io_buffer_index = 0;
    s->io_buffer_size = n * 512;
    int32_t prep = bm_prepare_buf(s->bus, s, n * 512);
    if (prep < n * 512) { // tabla PRD corta: sin IRQ, Active a 0
      s->status = READY_STAT | SEEK_STAT;
      ide_set_inactive(s, 0);
      return;
    }
    if (!ide_sect_range_ok(s, (uint64_t)sn, (uint64_t)n) || (s->dma_cmd == DMA_WRITE && s->ro)) {
      ide_dma_error(s);
      return;
    }
    ret = ide_sg_io(s, (uint64_t)sn * 512, s->dma_cmd == DMA_WRITE);
  }
}

static void ide_atapi_cmd_read_dma_cb(struct ide_if *s);

static void ide_run_dma_cb(struct ide_if *s) {
  switch (s->bus->dma_cb) {
  case CB_ATA: ide_dma_cb(s); break;
  case CB_ATAPI: ide_atapi_cmd_read_dma_cb(s); break;
  default: break;
  }
}

static void ide_start_dma(struct ide_if *s, int cb) {
  struct ide_bus *b = s->bus;
  s->io_buffer_index = 0;
  b->dma_cb = cb;
  b->cur_prd_last = 0;
  b->cur_prd_addr = 0;
  b->cur_prd_len = 0;
  if (b->bm_status & BM_STATUS_DMAING)
    ide_run_dma_cb(s);
}

static void ide_sector_start_dma(struct ide_if *s, int cmd) {
  s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
  s->io_buffer_size = 0;
  s->dma_cmd = cmd;
  ide_start_dma(s, CB_ATA);
}

static void bmdma_cmd_write(struct ide_bus *b, uint8_t val) {
  if ((val & BM_CMD_START) != (b->bm_cmd & BM_CMD_START)) {
    if (!(val & BM_CMD_START)) {
      // ide_cancel_dma_sync: aqui nada queda a medias
      b->bm_status &= ~BM_STATUS_DMAING;
    } else {
      b->cur_addr = b->bm_addr;
      if (!(b->bm_status & BM_STATUS_DMAING)) {
        b->bm_status |= BM_STATUS_DMAING;
        b->bm_cmd = val & 0x09;
        if (b->dma_cb)
          ide_run_dma_cb(ide_active(b));
        return;
      }
    }
  }
  b->bm_cmd = val & 0x09;
}

// ---------------------------------------------------------------------------
// ATAPI (hw/ide/atapi.c)
// ---------------------------------------------------------------------------
static int media_present(struct ide_if *s) { return !s->tray_open && s->nb_sectors > 0; }
static int media_is_dvd(struct ide_if *s) { return media_present(s) && s->nb_sectors > CD_MAX_SECTORS; }
static int media_is_cd(struct ide_if *s) { return media_present(s) && s->nb_sectors <= CD_MAX_SECTORS; }

static void lba_to_msf(uint8_t *buf, int lba) {
  lba += 150;
  buf[0] = (uint8_t)((lba / 75) / 60);
  buf[1] = (uint8_t)((lba / 75) % 60);
  buf[2] = (uint8_t)(lba % 75);
}

static void cd_data_to_raw(uint8_t *buf, int lba) {
  buf[0] = 0x00;
  memset(buf + 1, 0xff, 10);
  buf[11] = 0x00;
  buf += 12;
  lba_to_msf(buf, lba);
  buf[3] = 0x01;
  buf += 4 + 2048;
  memset(buf, 0, 288);
}

static int cd_read_sector(struct ide_if *s) {
  uint8_t *dst;
  if (s->cd_sector_size == 2048)
    dst = s->io_buffer;
  else if (s->cd_sector_size == 2352)
    dst = s->io_buffer + 16;
  else
    return -1;
  if (vm_pread(s->fd, dst, 2048, (off_t)s->lba * 2048) != 2048)
    return -1;
  dchk_buf(s, (uint64_t)s->lba * 2048, dst, 2048, 0);
  if (s->cd_sector_size == 2352)
    cd_data_to_raw(s->io_buffer, s->lba);
  s->lba++;
  s->io_buffer_index = 0;
  return 0;
}

static void ide_atapi_cmd_ok(struct ide_if *s) {
  s->error = 0;
  s->status = READY_STAT | SEEK_STAT;
  s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
  ide_transfer_stop(s);
  ide_bus_set_irq(s->bus);
}

static void ide_atapi_cmd_error(struct ide_if *s, int sense_key, int asc) {
  s->error = sense_key << 4;
  s->status = READY_STAT | ERR_STAT;
  s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
  s->sense_key = sense_key;
  s->asc = asc;
  ide_transfer_stop(s);
  ide_bus_set_irq(s->bus);
}

static void ide_atapi_io_error(struct ide_if *s) {
  ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
}

static int atapi_byte_count_limit(struct ide_if *s) {
  int bcl = s->lcyl | (s->hcyl << 8);
  return bcl == 0xffff ? 0xfffe : bcl;
}

static void ide_atapi_cmd_reply_end(struct ide_if *s) {
  while (s->packet_transfer_size > 0) {
    if (s->lba != -1 && s->io_buffer_index >= s->cd_sector_size) {
      if (cd_read_sector(s) < 0) {
        ide_atapi_io_error(s);
        return;
      }
    }
    int size;
    if (s->elementary_transfer_size > 0) {
      size = s->cd_sector_size - s->io_buffer_index;
      if (size > s->elementary_transfer_size)
        size = s->elementary_transfer_size;
    } else {
      s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO;
      ide_bus_set_irq(s->bus);
      int bcl = atapi_byte_count_limit(s);
      size = s->packet_transfer_size;
      if (size > bcl) {
        if (bcl & 1)
          bcl--;
        size = bcl;
      }
      s->lcyl = size & 0xff;
      s->hcyl = (size >> 8) & 0xff;
      s->elementary_transfer_size = size;
      if (s->lba != -1 && size > s->cd_sector_size - s->io_buffer_index)
        size = s->cd_sector_size - s->io_buffer_index;
    }
    if (size <= 0 || s->io_buffer_index + size > IDE_IOBUF) { // no deberia pasar
      ide_atapi_io_error(s);
      return;
    }
    s->packet_transfer_size -= size;
    s->elementary_transfer_size -= size;
    s->io_buffer_index += size;
    ide_transfer_start(s, s->io_buffer_index - size, size, ET_ATAPI_REPLY);
    return; // el huesped lee los datos; al acabar se vuelve aqui
  }
  ide_atapi_cmd_ok(s);
  ide_bus_set_irq(s->bus);
}

static void ide_atapi_cmd_reply(struct ide_if *s, int size, int max_size) {
  if (size > max_size)
    size = max_size;
  s->lba = -1;
  s->packet_transfer_size = size;
  s->io_buffer_size = size;
  s->elementary_transfer_size = 0;
  if (s->atapi_dma) {
    s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
    ide_start_dma(s, CB_ATAPI);
  } else {
    s->status = READY_STAT | SEEK_STAT;
    s->io_buffer_index = 0;
    ide_atapi_cmd_reply_end(s);
  }
}

static void ide_atapi_cmd_read_pio(struct ide_if *s, int lba, int nb, int ssize) {
  s->lba = lba;
  s->packet_transfer_size = nb * ssize;
  s->elementary_transfer_size = 0;
  s->io_buffer_index = ssize;
  s->cd_sector_size = ssize;
  ide_atapi_cmd_reply_end(s);
}

static void ide_atapi_cmd_check_status(struct ide_if *s) {
  s->error = MC_ERR | (SENSE_UNIT_ATTENTION << 4);
  s->status = ERR_STAT;
  s->nsector = 0;
  ide_bus_set_irq(s->bus);
}

static void ide_atapi_cmd_read_dma_cb(struct ide_if *s) {
  for (;;) {
    if (s->io_buffer_size > 0) {
      if (s->lba != -1) {
        int n;
        if (s->cd_sector_size == 2352) {
          n = 1;
          cd_data_to_raw(s->io_buffer, s->lba);
        } else {
          n = s->io_buffer_size >> 11;
        }
        s->lba += n;
      }
      s->packet_transfer_size -= s->io_buffer_size;
      if (bm_rw_buf(s->bus, s, 1) == 0) {
        ide_set_inactive(s, 0);
        return;
      }
    }
    if (s->packet_transfer_size <= 0) {
      s->status = READY_STAT | SEEK_STAT;
      s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
      ide_bus_set_irq(s->bus);
      ide_set_inactive(s, 0);
      return;
    }
    s->io_buffer_index = 0;
    int n, off;
    if (s->cd_sector_size == 2352) {
      n = 1;
      s->io_buffer_size = s->cd_sector_size;
      off = 16;
    } else {
      n = s->packet_transfer_size >> 11;
      if (n > IDE_DMA_BUF_SECTORS / 4)
        n = IDE_DMA_BUF_SECTORS / 4;
      s->io_buffer_size = n * 2048;
      off = 0;
    }
    if (vm_pread(s->fd, s->io_buffer + off, (size_t)n * 2048, (off_t)s->lba * 2048) != (ssize_t)n * 2048) {
      ide_atapi_io_error(s);
      ide_set_inactive(s, 0);
      return;
    }
    dchk_buf(s, (uint64_t)s->lba * 2048, s->io_buffer + off, (uint64_t)n * 2048, 0);
  }
}

static void ide_atapi_cmd_read(struct ide_if *s, int lba, int nb, int ssize) {
  if (s->atapi_dma) {
    s->lba = lba;
    s->packet_transfer_size = nb * ssize;
    s->io_buffer_size = 0;
    s->cd_sector_size = ssize;
    s->status = READY_STAT | SEEK_STAT | DRQ_STAT | BUSY_STAT;
    ide_start_dma(s, CB_ATAPI);
  } else {
    ide_atapi_cmd_read_pio(s, lba, nb, ssize);
  }
}

static int cdrom_read_toc(int nb_sectors, uint8_t *buf, int msf, int start_track) {
  if (start_track > 1 && start_track != 0xaa)
    return -1;
  uint8_t *q = buf + 2;
  *q++ = 1;
  *q++ = 1;
  if (start_track <= 1) {
    *q++ = 0;
    *q++ = 0x14;
    *q++ = 1;
    *q++ = 0;
    if (msf) {
      *q++ = 0;
      lba_to_msf(q, 0);
      q += 3;
    } else {
      put_be32(q, 0);
      q += 4;
    }
  }
  *q++ = 0;
  *q++ = 0x16;
  *q++ = 0xaa;
  *q++ = 0;
  if (msf) {
    *q++ = 0;
    lba_to_msf(q, nb_sectors);
    q += 3;
  } else {
    put_be32(q, (uint32_t)nb_sectors);
    q += 4;
  }
  int len = (int)(q - buf);
  put_be16(buf, (unsigned)(len - 2));
  return len;
}

static int cdrom_read_toc_raw(int nb_sectors, uint8_t *buf, int msf) {
  uint8_t *q = buf + 2;
  *q++ = 1;
  *q++ = 1;
  static const uint8_t a0[11] = {1, 0x14, 0, 0xa0, 0, 0, 0, 0, 1, 0x00, 0x00};
  static const uint8_t a1[11] = {1, 0x14, 0, 0xa1, 0, 0, 0, 0, 1, 0x00, 0x00};
  memcpy(q, a0, 11); q += 11;
  memcpy(q, a1, 11); q += 11;
  *q++ = 1; *q++ = 0x14; *q++ = 0; *q++ = 0xa2; *q++ = 0; *q++ = 0; *q++ = 0;
  if (msf) {
    *q++ = 0;
    lba_to_msf(q, nb_sectors);
    q += 3;
  } else {
    put_be32(q, (uint32_t)nb_sectors);
    q += 4;
  }
  *q++ = 1; *q++ = 0x14; *q++ = 0; *q++ = 1; *q++ = 0; *q++ = 0; *q++ = 0;
  if (msf) {
    *q++ = 0;
    lba_to_msf(q, 0);
    q += 3;
  } else {
    *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0;
  }
  int len = (int)(q - buf);
  put_be16(buf, (unsigned)(len - 2));
  return len;
}

static int validate_bcl(struct ide_if *s) {
  if (s->atapi_dma || atapi_byte_count_limit(s))
    return 1;
  ide_abort_command(s);
  return 0;
}

static void ide_atapi_cmd(struct ide_if *s) {
  uint8_t *buf = s->io_buffer;
  uint8_t op = buf[0];
  enum { ALLOW_UA = 1, CHECK_READY = 2, NONDATA = 4, CONDDATA = 8 };
  int flags, known = 1;
  switch (op) {
  case 0x00: flags = CHECK_READY | NONDATA; break;
  case 0x03: case 0x12: case 0x46: case 0x4a: flags = ALLOW_UA; break;
  case 0x1b: case 0x1e: case 0xbb: flags = NONDATA; break;
  case 0x25: case 0x28: case 0x43: case 0x51: case 0xa8: case 0xad: flags = CHECK_READY; break;
  case 0x2b: flags = CHECK_READY | NONDATA; break;
  case 0x5a: case 0xbd: flags = 0; break;
  case 0xbe: flags = CHECK_READY | CONDDATA; break;
  default: flags = 0; known = 0; break;
  }
  if (s->sense_key == SENSE_UNIT_ATTENTION && !(flags & ALLOW_UA)) {
    ide_atapi_cmd_check_status(s);
    return;
  }
  if (!(flags & ALLOW_UA) && !s->tray_open && s->nb_sectors && s->cdrom_changed) {
    if (s->cdrom_changed == 1) {
      ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
      s->cdrom_changed = 2;
    } else {
      ide_atapi_cmd_error(s, SENSE_UNIT_ATTENTION, ASC_MEDIUM_MAY_HAVE_CHANGED);
      s->cdrom_changed = 0;
    }
    return;
  }
  if ((flags & CHECK_READY) && !media_present(s)) {
    ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
    return;
  }
  if (known && !(flags & (NONDATA | CONDDATA)) && !validate_bcl(s))
    return;
  uint64_t total = s->nb_sectors >> 2;
  switch (op) {
  case 0x00: // TEST UNIT READY
    ide_atapi_cmd_ok(s);
    break;
  case 0x03: { // REQUEST SENSE
    int max_len = buf[4];
    memset(buf, 0, 18);
    buf[0] = 0x70 | 0x80;
    buf[2] = (uint8_t)s->sense_key;
    buf[7] = 10;
    buf[12] = (uint8_t)s->asc;
    if (s->sense_key == SENSE_UNIT_ATTENTION)
      s->sense_key = SENSE_NONE;
    ide_atapi_cmd_reply(s, 18, max_len);
    break;
  }
  case 0x12: { // INQUIRY
    int page = buf[2], max_len = buf[4];
    int idx = 0, size_idx, pre;
    if (buf[1] & 1) {
      pre = 4;
      size_idx = 3;
      buf[idx++] = 0x05;
      buf[idx++] = (uint8_t)page;
      buf[idx++] = 0;
      idx++;
      if (page == 0x00) {
        buf[idx++] = 0x00;
        buf[idx++] = 0x83;
      } else if (page == 0x83) {
        if (idx + 24 > max_len) {
          ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_DATA_PHASE_ERROR);
          return;
        }
        buf[idx++] = 0x02; buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 20;
        padstr8(buf + idx, 20, s->serial);
        idx += 20;
        if (idx + 72 <= max_len) {
          buf[idx++] = 0x02; buf[idx++] = 0x01; buf[idx++] = 0x00; buf[idx++] = 68;
          padstr8(buf + idx, 8, "ATA");
          idx += 8;
          padstr8(buf + idx, 40, s->model);
          idx += 40;
          padstr8(buf + idx, 20, s->serial);
          idx += 20;
        }
      } else {
        ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
      }
    } else {
      pre = 5;
      size_idx = 4;
      buf[0] = 0x05;
      buf[1] = 0x80;
      buf[2] = 0x00;
      buf[3] = 0x21;
      buf[5] = buf[6] = buf[7] = 0;
      padstr8(buf + 8, 8, "AURORA");
      padstr8(buf + 16, 16, "AURORA DVD-ROM");
      padstr8(buf + 32, 4, "2.5+");
      idx = 36;
    }
    buf[size_idx] = (uint8_t)(idx - pre);
    ide_atapi_cmd_reply(s, idx, max_len);
    break;
  }
  case 0x1b: { // START STOP UNIT
    int start = buf[4] & 1, loej = buf[4] & 2;
    if (buf[4] & 0xf0) {
      ide_atapi_cmd_ok(s);
      break;
    }
    if (loej) {
      if (!start && !s->tray_open && s->tray_locked) {
        ide_atapi_cmd_error(s, s->nb_sectors ? SENSE_NOT_READY : SENSE_ILLEGAL_REQUEST,
                            ASC_MEDIA_REMOVAL_PREVENTED);
        return;
      }
      if (s->tray_open != !start)
        s->tray_open = !start;
    }
    ide_atapi_cmd_ok(s);
    break;
  }
  case 0x1e: // PREVENT ALLOW MEDIUM REMOVAL
    s->tray_locked = buf[4] & 1;
    ide_atapi_cmd_ok(s);
    break;
  case 0x25: // READ CAPACITY
    put_be32(buf, (uint32_t)(total - 1));
    put_be32(buf + 4, 2048);
    ide_atapi_cmd_reply(s, 8, 8);
    break;
  case 0x28: case 0xa8: { // READ(10) / READ(12)
    uint32_t nb = op == 0x28 ? get_be16(buf + 7) : get_be32(buf + 6);
    if (nb == 0) {
      ide_atapi_cmd_ok(s);
      break;
    }
    uint32_t lba = get_be32(buf + 2);
    if (lba >= total || (uint64_t)lba + nb - 1 >= total) {
      ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
      break;
    }
    ide_atapi_cmd_read(s, (int)lba, (int)nb, 2048);
    break;
  }
  case 0x2b: { // SEEK
    uint32_t lba = get_be32(buf + 2);
    if (lba >= total)
      ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
    else
      ide_atapi_cmd_ok(s);
    break;
  }
  case 0x43: { // READ TOC/PMA/ATIP
    int max_len = (int)get_be16(buf + 7), format = buf[9] >> 6, msf = (buf[1] >> 1) & 1;
    int start_track = buf[6], len;
    switch (format) {
    case 0:
      len = cdrom_read_toc((int)total, buf, msf, start_track);
      if (len < 0)
        goto inv_field;
      ide_atapi_cmd_reply(s, len, max_len);
      break;
    case 1:
      memset(buf, 0, 12);
      buf[1] = 0x0a;
      buf[2] = 0x01;
      buf[3] = 0x01;
      ide_atapi_cmd_reply(s, 12, max_len);
      break;
    case 2:
      len = cdrom_read_toc_raw((int)total, buf, msf);
      ide_atapi_cmd_reply(s, len, max_len);
      break;
    default:
      goto inv_field;
    }
    break;
  }
  case 0x46: { // GET CONFIGURATION
    if (buf[2] != 0 || buf[3] != 0)
      goto inv_field;
    int max_len = (int)get_be16(buf + 7);
    if (max_len > 512)
      max_len = 512;
    memset(buf, 0, (size_t)max_len < 32 ? 32 : (size_t)max_len);
    if (media_is_dvd(s))
      put_be16(buf + 6, 0x0010);
    else if (media_is_cd(s))
      put_be16(buf + 6, 0x0008);
    buf[10] = 0x02 | 0x01;
    int len = 12;
    static const uint16_t prof[2] = {0x0010, 0x0008};
    for (int i = 0; i < 2; i++) {
      uint8_t *bp = buf + 12 + i * 4;
      put_be16(bp, prof[i]);
      bp[2] = (bp[0] == buf[6]) && (bp[1] == buf[7]);
      buf[11] += 4;
      len += 4;
    }
    put_be32(buf, (uint32_t)(len - 4));
    ide_atapi_cmd_reply(s, len, max_len);
    break;
  }
  case 0x4a: { // GET EVENT STATUS NOTIFICATION
    unsigned max_len = get_be16(buf + 7);
    if (!(buf[1] & 1)) {
      ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
      break;
    }
    uint8_t cls = buf[4];
    unsigned used;
    buf[3] = 1 << 4; // eventos soportados: medio
    buf[2] = 0;
    if (cls & (1 << 4)) {
      buf[2] |= 4;
      uint8_t ms = s->tray_open ? 1 : (s->nb_sectors ? 2 : 0), ec = 0;
      if (ms != 1 && s->new_media) {
        ec = 2;
        s->new_media = 0;
      }
      buf[4] = ec;
      buf[5] = ms;
      buf[6] = buf[7] = 0;
      used = 8;
    } else {
      buf[2] = 0x80;
      used = 4;
    }
    put_be16(buf, used - 4);
    ide_atapi_cmd_reply(s, (int)used, (int)max_len);
    break;
  }
  case 0x51: { // READ DISC INFORMATION
    if (buf[1] & 7)
      goto inv_field;
    int max_len = (int)get_be16(buf + 7);
    memset(buf, 0, 34);
    buf[1] = 32;
    buf[2] = 0xe;
    buf[3] = 1;
    buf[4] = 1;
    buf[5] = 1;
    buf[6] = 1;
    buf[7] = 0x20;
    ide_atapi_cmd_reply(s, 34, max_len);
    break;
  }
  case 0x5a: { // MODE SENSE(10)
    int max_len = (int)get_be16(buf + 7), action = buf[2] >> 6, code = buf[2] & 0x3f;
    if (action == 3) {
      ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_SAVING_PARAMETERS_NOT_SUPPORTED);
      break;
    }
    if (action != 0)
      goto inv_field;
    if (code == 0x01) {
      memset(buf, 0, 16);
      put_be16(buf, 16 - 2);
      buf[2] = 0x70;
      buf[8] = 0x01;
      buf[9] = 16 - 10;
      buf[11] = 0x05;
      ide_atapi_cmd_reply(s, 16, max_len);
    } else if (code == 0x0e) {
      memset(buf, 0, 24);
      put_be16(buf, 24 - 2);
      buf[2] = 0x70;
      buf[8] = 0x0e;
      buf[9] = 24 - 10;
      ide_atapi_cmd_reply(s, 24, max_len);
    } else if (code == 0x2a) {
      memset(buf, 0, 30);
      put_be16(buf, 30 - 2);
      buf[2] = 0x70;
      buf[8] = 0x2a;
      buf[9] = 30 - 10;
      buf[10] = 0x3b;
      buf[12] = 0x71;
      buf[13] = 3 << 5;
      buf[14] = (1 << 0) | (1 << 3) | (1 << 5);
      if (s->tray_locked)
        buf[14] |= 1 << 1;
      put_be16(buf + 16, 704);
      buf[19] = 2;
      put_be16(buf + 20, 512);
      put_be16(buf + 22, 704);
      ide_atapi_cmd_reply(s, 30, max_len);
    } else {
      goto inv_field;
    }
    break;
  }
  case 0xad: { // READ DVD STRUCTURE
    int max_len = (int)get_be16(buf + 8), media = buf[1], format = buf[7];
    if (format < 0xff) {
      if (media_is_cd(s)) {
        ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_INCOMPATIBLE_FORMAT);
        break;
      } else if (!media_present(s)) {
        goto inv_field;
      }
    }
    uint8_t pk[ATAPI_PACKET_SIZE];
    memcpy(pk, buf, sizeof(pk));
    memset(buf, 0, (size_t)(max_len > IDE_IOBUF ? IDE_IOBUF : max_len) < 64 ? 64 : (size_t)(max_len > IDE_IOBUF ? IDE_IOBUF : max_len));
    if (media != 0 || (format > 0x7f && format != 0xff))
      goto inv_field;
    int ret;
    if (format == 0x00) {
      if (pk[6] != 0)
        goto inv_field;
      if (total == 0) {
        ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_MEDIUM_NOT_PRESENT);
        break;
      }
      buf[4] = 1;
      buf[5] = 0xf;
      buf[6] = 1;
      buf[7] = 0;
      put_be32(buf + 8, 0);
      put_be32(buf + 12, (uint32_t)(total - 1));
      put_be32(buf + 16, (uint32_t)(total - 1));
      put_be16(buf, 2048 + 2);
      ret = 2048 + 4;
    } else if (format == 0x01) {
      buf[4] = 0;
      buf[5] = 0;
      put_be16(buf, 4 + 2);
      ret = 4 + 4;
    } else if (format == 0x04) {
      put_be16(buf, 2048 + 2);
      ret = 2048 + 4;
    } else if (format == 0xff) {
      buf[4] = 0x00; buf[5] = 0x40; put_be16(buf + 6, 2048 + 4);
      buf[8] = 0x01; buf[9] = 0x40; put_be16(buf + 10, 4 + 4);
      buf[12] = 0x03; buf[13] = 0x40; put_be16(buf + 14, 188 + 4);
      buf[16] = 0x04; buf[17] = 0x40; put_be16(buf + 18, 2048 + 4);
      put_be16(buf, 16 + 2);
      ret = 16 + 4;
    } else {
      goto inv_field;
    }
    ide_atapi_cmd_reply(s, ret, max_len);
    break;
  }
  case 0xbb: // SET CD SPEED
    ide_atapi_cmd_ok(s);
    break;
  case 0xbd: { // MECHANISM STATUS
    int max_len = (int)get_be16(buf + 8);
    put_be16(buf, 0);
    buf[2] = buf[3] = buf[4] = 0;
    buf[5] = 1;
    put_be16(buf + 6, 0);
    ide_atapi_cmd_reply(s, 8, max_len);
    break;
  }
  case 0xbe: { // READ CD
    uint32_t nb = ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 8) | buf[8];
    if (nb == 0) {
      ide_atapi_cmd_ok(s);
      break;
    }
    uint32_t lba = get_be32(buf + 2);
    if (lba >= total || (uint64_t)lba + nb - 1 >= total) {
      ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
      break;
    }
    int tr = buf[9] & 0xf8;
    if (tr == 0x00) {
      ide_atapi_cmd_ok(s);
      break;
    }
    if (!validate_bcl(s))
      break;
    if (tr == 0x10)
      ide_atapi_cmd_read(s, (int)lba, (int)nb, 2048);
    else if (tr == 0xf8)
      ide_atapi_cmd_read(s, (int)lba, (int)nb, 2352);
    else
      goto inv_field;
    break;
  }
  default:
    ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_ILLEGAL_OPCODE);
    break;
  }
  return;
inv_field:
  ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
}

// ---------------------------------------------------------------------------
// Comandos ATA (ide_bus_exec_cmd)
// ---------------------------------------------------------------------------
static void ide_cmd_lba48_transform(struct ide_if *s, int lba48) {
  s->lba48 = lba48;
  if (!lba48) {
    if (!s->nsector)
      s->nsector = 256;
  } else {
    if (!s->nsector && !s->hob_nsector)
      s->nsector = 65536;
    else
      s->nsector = (s->hob_nsector << 8) | s->nsector;
  }
}

static void ide_reset(struct ide_if *s) {
  if (s->reset_reverts) {
    s->reset_reverts = 0;
    s->heads = s->drive_heads;
    s->sectors = s->drive_sectors;
  }
  s->mult_sectors = MAX_MULT_SECTORS;
  s->feature = s->error = s->nsector = s->sector = s->lcyl = s->hcyl = 0;
  s->hob_feature = s->hob_sector = s->hob_nsector = s->hob_lcyl = s->hob_hcyl = 0;
  s->select = ATA_DEV_ALWAYS_ON;
  s->status = READY_STAT | SEEK_STAT;
  s->lba48 = 0;
  s->sense_key = 0;
  s->asc = 0;
  s->cdrom_changed = 0;
  s->packet_transfer_size = 0;
  s->elementary_transfer_size = 0;
  s->io_buffer_index = 0;
  s->cd_sector_size = 0;
  s->atapi_dma = 0;
  s->tray_locked = 0;
  s->tray_open = 0;
  s->io_buffer_size = 0;
  s->req_nb_sectors = 0;
  ide_set_signature(s);
  s->end_transfer = ET_DUMMY;
  ide_dummy_transfer_stop(s);
}

static void cmd_exec_dev_diagnostic(struct ide_if *s) {
  s->select = ATA_DEV_ALWAYS_ON;
  ide_set_signature(s);
  if (s->kind == IDE_CD) {
    s->status = 0;
    s->error = 0x01;
  } else {
    s->status = READY_STAT | SEEK_STAT;
    s->error = 0x01;
    ide_bus_set_irq(s->bus);
  }
}

static void ide_flush(struct ide_if *s) {
  if (s->kind == IDE_HD && !s->ro)
    fdatasync(s->fd);
  s->status = READY_STAT | SEEK_STAT;
  ide_bus_set_irq(s->bus);
}

// Devuelve 1 si el comando acaba ya (se completa en ide_exec_cmd).
static int ide_cmd_handler(struct ide_if *s, uint8_t cmd) {
  int hd = s->kind == IDE_HD;
  switch (cmd) {
  case 0x08: // DEVICE RESET (solo ATAPI)
    ide_transfer_halt(s);
    ide_reset(s);
    s->status = 0x00;
    return 0;
  case 0x10: // RECALIBRATE
  case 0x70: // SEEK
  case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6: // standby/idle/sleep
  case 0x94: case 0x95: case 0x96: case 0x97: case 0x99:
    return 1;
  case 0x20: case 0x21: case 0x24: // READ SECTORS (EXT)
    if (!hd) {
      if (s->kind == IDE_CD)
        ide_set_signature(s);
      ide_abort_command(s);
      return 1;
    }
    ide_cmd_lba48_transform(s, cmd == 0x24);
    s->req_nb_sectors = 1;
    ide_sector_read(s);
    return 0;
  case 0x30: case 0x31: case 0x34: case 0x3C: // WRITE SECTORS (EXT), WRITE VERIFY
    ide_cmd_lba48_transform(s, cmd == 0x34);
    s->req_nb_sectors = 1;
    s->status = SEEK_STAT | READY_STAT;
    ide_transfer_start(s, 0, 512, ET_SECTOR_WRITE);
    return 0;
  case 0xC4: case 0x29: // READ MULTIPLE (EXT)
    if (!s->mult_sectors) {
      ide_abort_command(s);
      return 1;
    }
    ide_cmd_lba48_transform(s, cmd == 0x29);
    s->req_nb_sectors = s->mult_sectors;
    ide_sector_read(s);
    return 0;
  case 0xC5: case 0x39: { // WRITE MULTIPLE (EXT)
    if (!s->mult_sectors) {
      ide_abort_command(s);
      return 1;
    }
    ide_cmd_lba48_transform(s, cmd == 0x39);
    s->req_nb_sectors = s->mult_sectors;
    int n = s->nsector < s->req_nb_sectors ? s->nsector : s->req_nb_sectors;
    s->status = SEEK_STAT | READY_STAT;
    ide_transfer_start(s, 0, 512 * n, ET_SECTOR_WRITE);
    return 0;
  }
  case 0xC8: case 0xC9: case 0x25: // READ DMA (EXT)
    ide_cmd_lba48_transform(s, cmd == 0x25);
    ide_sector_start_dma(s, DMA_READ);
    return 0;
  case 0xCA: case 0xCB: case 0x35: // WRITE DMA (EXT)
    ide_cmd_lba48_transform(s, cmd == 0x35);
    ide_sector_start_dma(s, DMA_WRITE);
    return 0;
  case 0x40: case 0x41: case 0x42: // VERIFY (EXT)
    ide_cmd_lba48_transform(s, cmd == 0x42);
    return 1;
  case 0x90: // EXECUTE DEVICE DIAGNOSTIC
    cmd_exec_dev_diagnostic(s);
    return 0;
  case 0x91: // INITIALIZE DEVICE PARAMETERS
    if (hd) {
      s->heads = (s->select & ATA_DEV_HS) + 1;
      s->sectors = s->nsector;
      ide_bus_set_irq(s->bus);
    } else {
      ide_abort_command(s);
    }
    return 1;
  case 0x98: case 0xE5: // CHECK POWER MODE
    s->nsector = 0xff;
    return 1;
  case 0xA0: // PACKET
    if (s->feature & 0x02) {
      ide_abort_command(s);
      return 1;
    }
    s->status = READY_STAT | SEEK_STAT;
    s->atapi_dma = s->feature & 1;
    if (s->atapi_dma)
      s->dma_cmd = DMA_ATAPI;
    s->nsector = 1;
    ide_transfer_start(s, 0, ATAPI_PACKET_SIZE, ET_ATAPI_CMD);
    return 0;
  case 0xA1: // IDENTIFY PACKET DEVICE
    ide_atapi_identify(s);
    s->status = READY_STAT | SEEK_STAT;
    ide_transfer_start(s, 0, 512, ET_STOP);
    ide_bus_set_irq(s->bus);
    return 0;
  case 0xB0: // SMART: sin datos que dar
    ide_abort_command(s);
    return 1;
  case 0xC6: // SET MULTIPLE MODE
    if ((s->nsector & 0xff) != 0 &&
        ((s->nsector & 0xff) > MAX_MULT_SECTORS || (s->nsector & (s->nsector - 1)) != 0))
      ide_abort_command(s);
    else
      s->mult_sectors = s->nsector & 0xff;
    return 1;
  case 0xE7: case 0xEA: // FLUSH CACHE (EXT)
    ide_flush(s);
    return 0;
  case 0xEC: // IDENTIFY DEVICE
    if (hd) {
      ide_identify(s);
      s->status = READY_STAT | SEEK_STAT;
      ide_transfer_start(s, 0, 512, ET_STOP);
      ide_bus_set_irq(s->bus);
      return 0;
    }
    if (s->kind == IDE_CD)
      ide_set_signature(s);
    ide_abort_command(s);
    return 1;
  case 0xEF: // SET FEATURES
    switch (s->feature) {
    case 0x02:
      s->wcache = 1;
      s->identify[85] = (1 << 14) | (1 << 5);
      return 1;
    case 0x82:
      s->wcache = 0;
      s->identify[85] = 1 << 14;
      ide_flush(s);
      return 0;
    case 0xcc: s->reset_reverts = 1; return 1;
    case 0x66: s->reset_reverts = 0; return 1;
    case 0xaa: case 0x55: case 0x05: case 0x85: case 0x69: case 0x67: case 0x96: case 0x9a:
    case 0x42: case 0xc2:
      return 1;
    case 0x03: {
      int val = s->nsector & 7;
      uint16_t *id = s->identify;
      switch (s->nsector >> 3) {
      case 0x00: case 0x01: id[62] = 0x07; id[63] = 0x07; id[88] = 0x3f; break;
      case 0x02: id[62] = (uint16_t)(0x07 | (1 << (val + 8))); id[63] = 0x07; id[88] = 0x3f; break;
      case 0x04: id[62] = 0x07; id[63] = (uint16_t)(0x07 | (1 << (val + 8))); id[88] = 0x3f; break;
      case 0x08: id[62] = 0x07; id[63] = 0x07; id[88] = (uint16_t)(0x3f | (1 << (val + 8))); break;
      default: ide_abort_command(s); return 1;
      }
      return 1;
    }
    default:
      ide_abort_command(s);
      return 1;
    }
  case 0xF8: case 0x27: // READ NATIVE MAX ADDRESS (EXT)
    if (s->nb_sectors == 0) {
      ide_abort_command(s);
      return 1;
    }
    ide_cmd_lba48_transform(s, cmd == 0x27);
    ide_set_sector(s, (int64_t)s->nb_sectors - 1);
    return 1;
  default:
    ide_abort_command(s);
    return 1;
  }
}

// Comandos permitidos por tipo (tabla ide_cmd_table de QEMU) y si fijan DSC.
static int ide_cmd_ok(int kind, uint8_t c, int *dsc) {
  *dsc = 0;
  switch (c) {
  case 0x08: case 0xA0: case 0xA1: return kind == IDE_CD;
  case 0x20: case 0x90: case 0xE7: case 0xEC: return 1;
  case 0xEF: *dsc = 1; return 1;
  case 0x10: case 0x40: case 0x41: case 0x42: case 0x70: case 0x91: case 0x98: case 0xE5:
  case 0xC6: case 0xB0: case 0xF8: case 0x27:
    *dsc = 1;
    return kind == IDE_HD;
  case 0x21: case 0x24: case 0x25: case 0x29: case 0x30: case 0x31: case 0x34: case 0x35:
  case 0x39: case 0x3C: case 0x94: case 0x95: case 0x96: case 0x97: case 0x99: case 0xC4:
  case 0xC5: case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xE0: case 0xE1: case 0xE2:
  case 0xE3: case 0xE6: case 0xEA:
    return kind == IDE_HD;
  default:
    return 0;
  }
}

static void ide_exec_cmd(struct ide_bus *b, uint8_t val) {
  struct ide_if *s = ide_active(b);
  if (s != &b->ifs[0] && s->kind == IDE_NONE)
    return; // esclavo inexistente
  if (s->status & (BUSY_STAT | DRQ_STAT))
    if (val != 0x08 || s->kind != IDE_CD)
      return;
  int dsc;
  if (s->kind == IDE_NONE || !ide_cmd_ok(s->kind, val, &dsc)) {
    ide_abort_command(s);
    ide_bus_set_irq(b);
    return;
  }
  s->status = READY_STAT | BUSY_STAT;
  s->error = 0;
  if (ide_cmd_handler(s, val)) {
    s->status &= ~BUSY_STAT;
    if (dsc && !s->error)
      s->status |= SEEK_STAT;
    ide_bus_set_irq(b);
  }
}

// ---------------------------------------------------------------------------
// Puertos
// ---------------------------------------------------------------------------
static int ide_bus_empty(struct ide_bus *b) { return b->ifs[0].kind == IDE_NONE && b->ifs[1].kind == IDE_NONE; }
static int ide_absent(struct ide_bus *b, struct ide_if *s) { return s != &b->ifs[0] && s->kind == IDE_NONE; }

static uint32_t ide_data_read(struct ide_bus *b, int size) {
  struct ide_if *s = ide_active(b);
  if (!(s->status & DRQ_STAT) || !ide_is_pio_out(s))
    return 0;
  if (s->data_ptr + size > s->data_end)
    return 0;
  uint32_t v = 0;
  memcpy(&v, s->io_buffer + s->data_ptr, (size_t)size);
  s->data_ptr += size;
  if (s->data_ptr >= s->data_end) {
    s->status &= ~DRQ_STAT;
    ide_end_transfer(s);
  }
  return v;
}

// REP INSW/INSD del puerto de datos en bloque: copia a dst hasta n bytes (en
// elementos de `size`) mientras el dispositivo tenga datos (DRQ), cruzando
// sectores igual que ide_data_read elemento a elemento. Devuelve los bytes
// copiados; lo que falte lo hace el camino lento (que da 0 sin DRQ).
static unsigned ide_data_read_bulk(uint16_t port, uint8_t *dst, unsigned n, int size) {
  struct ide_bus *b = &g_ide[(port & 0x1F0) == 0x170];
  unsigned done = 0;
  while (n - done >= (unsigned)size) {
    struct ide_if *s = ide_active(b);
    if (!(s->status & DRQ_STAT) || !ide_is_pio_out(s) || s->data_ptr + size > s->data_end)
      break;
    unsigned c = (unsigned)(s->data_end - s->data_ptr);
    if (c > n - done)
      c = n - done;
    c -= c % (unsigned)size;
    if (!c)
      break;
    memcpy(dst + done, s->io_buffer + s->data_ptr, c);
    s->data_ptr += (int)c;
    done += c;
    if (s->data_ptr >= s->data_end) {
      s->status &= ~DRQ_STAT;
      ide_end_transfer(s);
    }
  }
  return done;
}

static void ide_data_write(struct ide_bus *b, uint32_t v, int size) {
  struct ide_if *s = ide_active(b);
  if (!(s->status & DRQ_STAT) || ide_is_pio_out(s))
    return;
  if (s->data_ptr + size > s->data_end)
    return;
  memcpy(s->io_buffer + s->data_ptr, &v, (size_t)size);
  s->data_ptr += size;
  if (s->data_ptr >= s->data_end) {
    s->status &= ~DRQ_STAT;
    ide_end_transfer(s);
  }
}

static uint8_t ide_ioport_read(struct ide_bus *b, unsigned reg) {
  struct ide_if *s = ide_active(b);
  int hob = b->cmd & IDE_CTRL_HOB, ret;
  switch (reg) {
  case 0: ret = (int)(ide_data_read(b, 2) & 0xff); break;
  case 1: ret = (ide_bus_empty(b) || ide_absent(b, s)) ? 0 : hob ? s->hob_feature : s->error; break;
  case 2: ret = ide_bus_empty(b) ? 0 : hob ? s->hob_nsector : s->nsector & 0xff; break;
  case 3: ret = ide_bus_empty(b) ? 0 : hob ? s->hob_sector : s->sector; break;
  case 4: ret = ide_bus_empty(b) ? 0 : hob ? s->hob_lcyl : s->lcyl; break;
  case 5: ret = ide_bus_empty(b) ? 0 : hob ? s->hob_hcyl : s->hcyl; break;
  case 6: ret = ide_bus_empty(b) ? 0 : s->select; break;
  default:
    ret = (ide_bus_empty(b) || ide_absent(b, s)) ? 0 : s->status;
    ide_bus_lower_irq(b);
    break;
  }
  return (uint8_t)ret;
}

static void ide_ioport_write(struct ide_bus *b, unsigned reg, uint8_t val) {
  struct ide_if *s = ide_active(b);
  if (reg != 7 && (s->status & (BUSY_STAT | DRQ_STAT)))
    return;
  if (reg >= 1 && reg <= 6)
    b->cmd &= ~IDE_CTRL_HOB;
  for (int i = 0; i < 2; i++) {
    struct ide_if *f = &b->ifs[i];
    switch (reg) {
    case 1: f->hob_feature = f->feature; f->feature = val; break;
    case 2: f->hob_nsector = f->nsector & 0xff; f->nsector = val; break;
    case 3: f->hob_sector = f->sector; f->sector = val; break;
    case 4: f->hob_lcyl = f->lcyl; f->lcyl = val; break;
    case 5: f->hob_hcyl = f->hcyl; f->hcyl = val; break;
    case 6: f->select = val | ATA_DEV_ALWAYS_ON; break;
    default: break;
    }
  }
  if (reg == 6)
    b->unit = (val & ATA_DEV_SELECT) ? 1 : 0;
  if (reg == 7) {
    b->cmd &= ~IDE_CTRL_HOB;
    ide_bus_lower_irq(b);
    ide_exec_cmd(b, val);
  }
}

static uint8_t ide_status_read(struct ide_bus *b) {
  struct ide_if *s = ide_active(b);
  return (uint8_t)((ide_bus_empty(b) || ide_absent(b, s)) ? 0 : s->status);
}

static void ide_perform_srst(struct ide_if *s) {
  s->status |= BUSY_STAT;
  ide_transfer_halt(s);
  ide_reset(s);
  cmd_exec_dev_diagnostic(s);
}

static void ide_ctrl_write(struct ide_bus *b, uint8_t val) {
  int start = !(b->cmd & IDE_CTRL_RESET) && (val & IDE_CTRL_RESET);
  b->cmd = val;
  if (start) {
    // QEMU hace el reinicio en un "bottom half" justo despues de la escritura.
    b->bm_status &= ~BM_STATUS_DMAING;
    b->dma_cb = CB_NONE;
    for (int i = 0; i < 2; i++)
      ide_perform_srst(&b->ifs[i]);
    b->cmd &= ~IDE_CTRL_RESET;
  }
}

// ---------------------------------------------------------------------------
// Configuracion
// ---------------------------------------------------------------------------
static int ide_attach(int bus, int unit, int kind, const char *path) {
  struct ide_if *s = &g_ide[bus].ifs[unit];
  s->ro = kind == IDE_CD;
  s->fd = open(path, (s->ro ? O_RDONLY : O_RDWR) | O_CLOEXEC);
  if (s->fd < 0 && !s->ro) {
    s->ro = 1;
    s->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (s->fd >= 0)
      fprintf(stderr, "aurora-vm: aviso: %s abierto solo lectura\n", path);
  }
  struct stat st;
  if (s->fd < 0 || fstat(s->fd, &st) < 0) {
    fprintf(stderr, "aurora-vm: no puedo abrir %s: %s\n", path, strerror(errno));
    return -1;
  }
  uint64_t bytes = (uint64_t)st.st_size;
  if (S_ISBLK(st.st_mode)) {
    off_t e = lseek(s->fd, 0, SEEK_END);
    bytes = e > 0 ? (uint64_t)e : 0;
  }
  if (bytes < (kind == IDE_CD ? 2048u : 512u)) {
    fprintf(stderr, "aurora-vm: %s esta vacio\n", path);
    return -1;
  }
  s->kind = kind;
  s->nb_sectors = bytes / 512;
  if (kind == IDE_CD)
    s->nb_sectors &= ~3ULL; // sectores de 2048 completos
  uint64_t cyl = s->nb_sectors / (16 * 63);
  s->cylinders = cyl > 16383 ? 16383 : cyl < 2 ? 2 : (int)cyl;
  s->heads = s->drive_heads = 16;
  s->sectors = s->drive_sectors = 63;
  s->wcache = 1;
  snprintf(s->serial, sizeof(s->serial), "AVM%05d", bus * 2 + unit + 1);
  snprintf(s->model, sizeof(s->model), "%s", kind == IDE_CD ? "AURORA DVD-ROM" : "AURORA HARDDISK");
  g_ide_on = 1;
  return 0;
}

static void ide_init(void) {
  for (int b = 0; b < 2; b++) {
    struct ide_bus *bus = &g_ide[b];
    bus->irq = 14 + b;
    for (int u = 0; u < 2; u++) {
      bus->ifs[u].bus = bus;
      bus->ifs[u].unit = u;
      if (bus->ifs[u].kind == IDE_NONE)
        bus->ifs[u].fd = -1;
    }
  }
}

static void ide_reset_all(void) {
  for (int b = 0; b < 2; b++) {
    struct ide_bus *bus = &g_ide[b];
    bus->unit = 0;
    bus->cmd = 0;
    bus->irq_level = 0;
    bus->bm_cmd = 0;
    bus->bm_status = 0;
    bus->bm_addr = bus->cur_addr = 0;
    bus->dma_cb = CB_NONE;
    for (int u = 0; u < 2; u++) {
      bus->ifs[u].identify_set = 0;
      ide_reset(&bus->ifs[u]); // estado del maestro de bus a 0, como QEMU (SeaBIOS no lo toca)
    }
  }
}

static int ide_port(uint16_t p) {
  return g_ide_on && ((p >= 0x1F0 && p <= 0x1F7) || p == 0x3F6 || (p >= 0x170 && p <= 0x177) || p == 0x376);
}

static uint32_t ide_in(uint16_t p, int size) {
  struct ide_bus *b = &g_ide[(p & 0x1F0) == 0x170 || p == 0x376];
  if (p == 0x3F6 || p == 0x376)
    return ide_status_read(b);
  unsigned reg = p & 7;
  if (reg == 0 && size >= 2)
    return ide_data_read(b, size);
  uint32_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint32_t)ide_ioport_read(b, (reg + (unsigned)i) & 7) << (8 * i);
  return v;
}

static void ide_out(uint16_t p, uint32_t v, int size) {
  struct ide_bus *b = &g_ide[(p & 0x1F0) == 0x170 || p == 0x376];
  if (p == 0x3F6 || p == 0x376) {
    ide_ctrl_write(b, (uint8_t)v);
    return;
  }
  unsigned reg = p & 7;
  if (reg == 0 && size >= 2) {
    ide_data_write(b, v, size);
    return;
  }
  for (int i = 0; i < size; i++)
    ide_ioport_write(b, (reg + (unsigned)i) & 7, (uint8_t)(v >> (8 * i)));
}

// Maestro de bus: canal 0 en +0..+7, canal 1 en +8..+15
static uint32_t ide_bm_in(unsigned off, int size) {
  uint32_t v = 0;
  for (int i = 0; i < size; i++) {
    unsigned k = off + (unsigned)i;
    struct ide_bus *b = &g_ide[(k >> 3) & 1];
    unsigned r = k & 7;
    uint8_t x = 0xff;
    if (r == 0) x = b->bm_cmd;
    else if (r == 2) x = b->bm_status;
    else if (r >= 4) x = (uint8_t)(b->bm_addr >> (8 * (r - 4)));
    v |= (uint32_t)x << (8 * i);
  }
  return v;
}

static void ide_bm_out(unsigned off, uint32_t v, int size) {
  for (int i = 0; i < size; i++) {
    unsigned k = off + (unsigned)i;
    struct ide_bus *b = &g_ide[(k >> 3) & 1];
    unsigned r = k & 7;
    uint8_t x = (uint8_t)(v >> (8 * i));
    if (r == 0) {
      bmdma_cmd_write(b, x);
    } else if (r == 2) {
      b->bm_status = (uint8_t)((x & 0x60) | (b->bm_status & BM_STATUS_DMAING) | (b->bm_status & ~x & 0x06));
    } else if (r >= 4) {
      unsigned sh = 8 * (r - 4);
      b->bm_addr = (b->bm_addr & ~(0xFFu << sh)) | ((uint32_t)x << sh);
      b->bm_addr &= ~3u;
    }
  }
}

#endif
