// kernel/iommu.c
//
// Intel VT-d (L9.1). Ver iommu.h para la politica.
//
// Estructura:
//   DMAR (ACPI)  -> unidades DRHD con sus ambitos (dispositivos/puentes) y
//                   RMRR.
//   Por unidad   -> tabla raiz + una tabla de contexto por bus con
//                   dispositivos. Todos los contextos nacen en pass-through
//                   (T=10) y el dominio privado de un dispositivo cambia su
//                   entrada a traduccion de segundo nivel.
//   Por dominio  -> tablas de paginas de 3 o 4 niveles (4 KiB) y asignador de
//                   IOVA de arriba hacia abajo con lista de huecos libres.
//
// Invalidacion por registros (sin cola): CCMD/IOTLB global tras cambiar
// contextos, IOTLB por dominio tras cambiar paginas.

#include "iommu.h"
#include "acpi.h"
#include "cmdline.h"
#include "heap.h"
#include "idt.h"
#include "klog.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"

// ---------------------------------------------------------------------------
// Registros VT-d.
// ---------------------------------------------------------------------------
#define R_VER 0x00
#define R_CAP 0x08
#define R_ECAP 0x10
#define R_GCMD 0x18
#define R_GSTS 0x1C
#define R_RTADDR 0x20
#define R_CCMD 0x28
#define R_FSTS 0x34
#define R_FECTL 0x38
#define R_FEDATA 0x3C
#define R_FEADDR 0x40
#define R_FEUADDR 0x44

#define G_TE (1u << 31)
#define G_SRTP (1u << 30)
#define G_WBF (1u << 27)
#define G_QIE (1u << 26)
#define G_IRE (1u << 25)
// Bits de GSTS que reflejan habilitaciones persistentes (el resto son
// estados de ordenes de un solo disparo y no se pueden reescribir en GCMD).
#define G_PERSISTENT 0x96FFFFFFu

#define CAP_ND(c) ((unsigned)((c) & 7))
#define CAP_RWBF(c) (((c) >> 4) & 1)
#define CAP_CM(c) (((c) >> 7) & 1)
#define CAP_SAGAW(c) ((unsigned)(((c) >> 8) & 0x1f))
#define CAP_MGAW(c) ((unsigned)(((c) >> 16) & 0x3f) + 1)
#define CAP_FRO(c) ((unsigned)(((c) >> 24) & 0x3ff))
#define CAP_NFR(c) ((unsigned)(((c) >> 40) & 0xff) + 1)
#define CAP_DWD(c) (((c) >> 54) & 1)
#define CAP_DRD(c) (((c) >> 55) & 1)

#define ECAP_C(e) ((unsigned)((e) & 1))
#define ECAP_PT(e) ((unsigned)(((e) >> 6) & 1))
#define ECAP_IRO(e) ((unsigned)(((e) >> 8) & 0x3ff))

#define MAX_UNITS 8
#define MAX_SCOPES 12
#define MAX_RMRR 16
#define MSI_BASE_ADDR 0xFEE00000ULL
#define MSI_HOLE_END 0xFEF00000ULL
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

// ---------------------------------------------------------------------------
// Estructuras.
// ---------------------------------------------------------------------------
typedef struct {
  uint8_t type; // 1 = endpoint, 2 = puente (todo lo que cuelga de el)
  bool ok;
  uint8_t bus, devfn;       // endpoint: el dispositivo; puente: el puente
  uint8_t sec_bus, sub_bus; // puente: rango de buses que cubre
} scope_t;

typedef struct iommu_unit {
  int idx;
  uint64_t reg_phys;
  volatile uint8_t *reg;
  uint16_t segment;
  bool pci_all;
  uint64_t cap, ecap;
  uint32_t gcmd;
  unsigned levels; // 3 o 4
  unsigned addr_bits;
  bool coherent;
  bool enabled;
  bool skip; // no se toca (IR activa, sin capacidades, ...)
  uint64_t iro;
  uint64_t root_phys;
  uint64_t ctx_phys[256]; // por bus; 0 = sin tabla
  spinlock_t reg_lock;
  spinlock_t did_lock;
  spinlock_t fault_lock; // serializa la IRQ de fallos con iommu_fault_poll
  uint8_t *did_map; // bitmap de DID usados
  unsigned did_max;
  int nscopes;
  scope_t scopes[MAX_SCOPES];
} iommu_unit_t;

typedef struct {
  uint64_t base, limit; // limite inclusivo
  int nscopes;
  scope_t scopes[MAX_SCOPES];
} rmrr_t;

typedef struct free_range {
  uint64_t start, end; // [start, end)
  struct free_range *next;
} free_range_t;

struct iommu_domain {
  iommu_unit_t *unit;
  uint16_t bdf;
  uint16_t did;
  unsigned levels;
  uint64_t limit; // IOVA maxima (inclusiva)
  uint64_t pgd_phys;
  spinlock_t lock;
  free_range_t *free;
  uint64_t tables;          // paginas de tablas reservadas (excluido pgd)
  uint64_t table_list_head; // lista enlazada de paginas de tabla (phys)
  volatile uint64_t maps, unmaps, pages_live, faults;
  volatile uint64_t last_fault_iova;
  volatile uint8_t last_fault_reason;
  volatile uint8_t quarantined;
  uint8_t bm_parked; // iommu_suspend le quito el bus master
};

static iommu_unit_t g_units[MAX_UNITS];
static int g_nunits;
static rmrr_t g_rmrr[MAX_RMRR];
static int g_nrmrr;
static iommu_domain_t **g_dom; // por bdf (65536 punteros)
static spinlock_t g_dom_lock;
static spinlock_t g_create_lock; // una sola creacion de dominio a la vez
static bool g_initialized;
static bool g_active;
static bool g_igfx_off;
static int g_fault_vector = -1;
static volatile uint64_t g_faults, g_unattributed;
static volatile int g_fault_log_budget = 32;
static int g_ndomains;
static uint64_t g_fault_limit = 10000;

// ---------------------------------------------------------------------------
// Utilidades.
// ---------------------------------------------------------------------------
static inline uint32_t rd32(iommu_unit_t *u, unsigned off) {
  return *(volatile uint32_t *)(u->reg + off);
}
static inline void wr32(iommu_unit_t *u, unsigned off, uint32_t v) {
  *(volatile uint32_t *)(u->reg + off) = v;
}
static inline uint64_t rd64(iommu_unit_t *u, unsigned off) {
  return *(volatile uint64_t *)(u->reg + off);
}
static inline void wr64(iommu_unit_t *u, unsigned off, uint64_t v) {
  *(volatile uint64_t *)(u->reg + off) = v;
}

static inline void cflush(const void *p) {
  __asm__ volatile("clflush (%0)" ::"r"(p) : "memory");
}

// Si la unidad no recorre las tablas coherentemente con la cache de la CPU,
// hay que escribir las lineas a memoria tras tocar una entrada.
static inline void flush_entry(iommu_unit_t *u, const void *p, unsigned len) {
  if (u->coherent) {
    __asm__ volatile("" ::: "memory");
    return;
  }
  const uint8_t *a = (const uint8_t *)((uint64_t)p & ~63ULL);
  const uint8_t *e = (const uint8_t *)p + len;
  for (; a < e; a += 64)
    cflush(a);
  __asm__ volatile("mfence" ::: "memory");
}

static uint64_t alloc_zeroed_page(void) {
  uint64_t p = pmm_alloc_page();
  if (p)
    memset(phys_to_virt(p), 0, 4096);
  return p;
}

// Espera hasta que (reg & mask) == want. 0 si llega, -1 si se agota.
static int wait_reg32(iommu_unit_t *u, unsigned off, uint32_t mask,
                      uint32_t want) {
  for (uint64_t i = 0; i < 20000000ULL; i++) {
    if ((rd32(u, off) & mask) == want)
      return 0;
    __asm__ volatile("pause");
  }
  return -1;
}
static int wait_reg64_clear(iommu_unit_t *u, unsigned off, uint64_t bit) {
  for (uint64_t i = 0; i < 20000000ULL; i++) {
    if (!(rd64(u, off) & bit))
      return 0;
    __asm__ volatile("pause");
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Acceso PCI auxiliar (segmento 0).
// ---------------------------------------------------------------------------
static bool pci_present(uint8_t bus, uint8_t devfn) {
  return (pci_read_config_word(bus, devfn >> 3, devfn & 7, 0) != 0xFFFF);
}

// ---------------------------------------------------------------------------
// Lectura de DMAR.
// ---------------------------------------------------------------------------
static void resolve_scope(scope_t *s, uint8_t start_bus, const uint8_t *path,
                          int npairs) {
  uint8_t bus = start_bus;
  s->ok = false;
  for (int i = 0; i < npairs; i++) {
    uint8_t dev = path[2 * i], fn = path[2 * i + 1];
    uint8_t devfn = (uint8_t)((dev << 3) | (fn & 7));
    if (!pci_present(bus, devfn))
      return; // el dispositivo no existe (el firmware lista de mas)
    if (i == npairs - 1) {
      s->bus = bus;
      s->devfn = devfn;
      if (s->type == 2) {
        s->sec_bus = pci_read_config_byte(bus, dev, fn & 7, 0x19);
        s->sub_bus = pci_read_config_byte(bus, dev, fn & 7, 0x1A);
      }
      s->ok = true;
      return;
    }
    // Puente intermedio: seguir hacia su bus secundario.
    bus = pci_read_config_byte(bus, dev, fn & 7, 0x19);
  }
}

static int parse_scopes(const uint8_t *p, uint32_t len, scope_t *out,
                        int max) {
  int n = 0;
  uint32_t off = 0;
  while (off + 6 <= len) {
    uint8_t type = p[off], slen = p[off + 1];
    if (slen < 6 || off + slen > len)
      break;
    if ((type == 1 || type == 2) && n < max) {
      scope_t *s = &out[n];
      memset(s, 0, sizeof(*s));
      s->type = type;
      resolve_scope(s, p[off + 5], p + off + 6, (slen - 6) / 2);
      if (s->ok)
        n++;
    }
    off += slen;
  }
  return n;
}

static bool scope_matches(const scope_t *s, uint8_t bus, uint8_t devfn) {
  if (!s->ok)
    return false;
  if (s->type == 1)
    return s->bus == bus && s->devfn == devfn;
  // Puente: el propio puente o cualquier dispositivo en su rango de buses.
  if (s->bus == bus && s->devfn == devfn)
    return true;
  return bus >= s->sec_bus && bus <= s->sub_bus && s->sec_bus != 0;
}

static iommu_unit_t *unit_for_bdf(uint16_t bdf) {
  uint8_t bus = bdf >> 8, devfn = bdf & 0xFF;
  iommu_unit_t *all = NULL;
  for (int i = 0; i < g_nunits; i++) {
    iommu_unit_t *u = &g_units[i];
    if (u->segment != 0)
      continue;
    if (u->pci_all) {
      all = u;
      continue;
    }
    for (int k = 0; k < u->nscopes; k++)
      if (scope_matches(&u->scopes[k], bus, devfn))
        return u;
  }
  return all;
}

static void parse_dmar(void) {
  uint32_t len = 0;
  const uint8_t *t = (const uint8_t *)acpi_find_table("DMAR", &len);
  if (!t || len < 48)
    return;
  LOG_INFO("[IOMMU] DMAR: HAW=%u flags=0x%x", t[36] + 1u, t[37]);
  uint32_t off = 48;
  while (off + 4 <= len) {
    uint16_t type = *(const uint16_t *)(t + off);
    uint16_t slen = *(const uint16_t *)(t + off + 2);
    if (slen < 4 || off + slen > len)
      break;
    if (type == 0 && slen >= 16 && g_nunits < MAX_UNITS) { // DRHD
      iommu_unit_t *u = &g_units[g_nunits];
      memset(u, 0, sizeof(*u));
      u->idx = g_nunits;
      u->pci_all = t[off + 4] & 1;
      u->segment = *(const uint16_t *)(t + off + 6);
      u->reg_phys = *(const uint64_t *)(t + off + 8);
      u->nscopes = parse_scopes(t + off + 16, slen - 16, u->scopes, MAX_SCOPES);
      LOG_INFO("[IOMMU] DRHD%d: regs=0x%llx seg=%u %s ambitos=%d", g_nunits,
               (unsigned long long)u->reg_phys, u->segment,
               u->pci_all ? "PCI_ALL" : "especifico", u->nscopes);
      g_nunits++;
    } else if (type == 1 && slen >= 24 && g_nrmrr < MAX_RMRR) { // RMRR
      rmrr_t *r = &g_rmrr[g_nrmrr];
      memset(r, 0, sizeof(*r));
      uint16_t seg = *(const uint16_t *)(t + off + 6);
      r->base = *(const uint64_t *)(t + off + 8);
      r->limit = *(const uint64_t *)(t + off + 16);
      r->nscopes = parse_scopes(t + off + 24, slen - 24, r->scopes, MAX_SCOPES);
      if (seg == 0 && r->limit >= r->base) {
        LOG_INFO("[IOMMU] RMRR: 0x%llx-0x%llx ambitos=%d",
                 (unsigned long long)r->base, (unsigned long long)r->limit,
                 r->nscopes);
        g_nrmrr++;
      }
    }
    off += slen;
  }
}

// ---------------------------------------------------------------------------
// Invalidaciones (por registros).
// ---------------------------------------------------------------------------
static void flush_write_buffer(iommu_unit_t *u) {
  if (!CAP_RWBF(u->cap))
    return;
  wr32(u, R_GCMD, u->gcmd | G_WBF);
  wait_reg32(u, R_GSTS, G_WBF, 0);
}

static void unit_flush_ctx_global(iommu_unit_t *u) {
  unsigned long fl = spin_lock_irqsave(&u->reg_lock);
  flush_write_buffer(u);
  wr64(u, R_CCMD, (1ULL << 63) | (1ULL << 61)); // ICC | CIRG=global
  if (wait_reg64_clear(u, R_CCMD, 1ULL << 63))
    LOG_ERR("[IOMMU] DRHD%d: invalidacion de contextos sin respuesta", u->idx);
  spin_unlock_irqrestore(&u->reg_lock, fl);
}

static void unit_flush_iotlb(iommu_unit_t *u, int did /* -1 = global */) {
  unsigned long fl = spin_lock_irqsave(&u->reg_lock);
  flush_write_buffer(u);
  uint64_t v = 1ULL << 63; // IVT
  v |= did < 0 ? (1ULL << 60) : ((2ULL << 60) | ((uint64_t)did << 32));
  if (CAP_DRD(u->cap))
    v |= 1ULL << 49;
  if (CAP_DWD(u->cap))
    v |= 1ULL << 48;
  wr64(u, (unsigned)u->iro + 8, v);
  if (wait_reg64_clear(u, (unsigned)u->iro + 8, 1ULL << 63))
    LOG_ERR("[IOMMU] DRHD%d: invalidacion de IOTLB sin respuesta", u->idx);
  spin_unlock_irqrestore(&u->reg_lock, fl);
}

// ---------------------------------------------------------------------------
// Tabla raiz / contexto.
// ---------------------------------------------------------------------------
typedef struct {
  volatile uint64_t lo, hi;
} ctx_entry_t;

static ctx_entry_t *ctx_table_for_bus(iommu_unit_t *u, uint8_t bus,
                                      bool create) {
  if (!u->ctx_phys[bus]) {
    if (!create)
      return NULL;
    uint64_t p = alloc_zeroed_page();
    if (!p)
      return NULL;
    u->ctx_phys[bus] = p;
    uint64_t *root = (uint64_t *)phys_to_virt(u->root_phys);
    root[2 * bus] = p | 1;
    root[2 * bus + 1] = 0;
    flush_entry(u, &root[2 * bus], 16);
    flush_entry(u, phys_to_virt(p), 4096);
  }
  return (ctx_entry_t *)phys_to_virt(u->ctx_phys[bus]);
}

static void ctx_set_passthrough(iommu_unit_t *u, ctx_entry_t *e, int did) {
  e->hi = (uint64_t)(u->levels - 2) | ((uint64_t)did << 8);
  e->lo = (2ULL << 2) | 1; // T=pass-through, P=1
  flush_entry(u, e, 16);
}

static int did_alloc(iommu_unit_t *u) {
  unsigned long fl = spin_lock_irqsave(&u->did_lock);
  int r = -1;
  // DID 0 esta reservado (CM) y DID 1 es el compartido de pass-through.
  for (unsigned i = 2; i < u->did_max; i++) {
    if (!(u->did_map[i >> 3] & (1u << (i & 7)))) {
      u->did_map[i >> 3] |= (uint8_t)(1u << (i & 7));
      r = (int)i;
      break;
    }
  }
  spin_unlock_irqrestore(&u->did_lock, fl);
  return r;
}
static void did_free(iommu_unit_t *u, int did) {
  unsigned long fl = spin_lock_irqsave(&u->did_lock);
  u->did_map[did >> 3] &= (uint8_t)~(1u << (did & 7));
  spin_unlock_irqrestore(&u->did_lock, fl);
}

// ---------------------------------------------------------------------------
// Habilitacion de una unidad.
// ---------------------------------------------------------------------------
static int unit_hw_enable(iommu_unit_t *u) {
  uint32_t gsts = rd32(u, R_GSTS);
  u->gcmd = gsts & G_PERSISTENT;

  // Si el firmware dejo la traduccion o la cola de invalidacion activas, se
  // apagan para empezar de cero (como hace Linux).
  if (u->gcmd & G_TE) {
    u->gcmd &= ~G_TE;
    wr32(u, R_GCMD, u->gcmd);
    if (wait_reg32(u, R_GSTS, G_TE, 0))
      return -1;
  }
  if (u->gcmd & G_QIE) {
    u->gcmd &= ~G_QIE;
    wr32(u, R_GCMD, u->gcmd);
    if (wait_reg32(u, R_GSTS, G_QIE, 0))
      return -1;
  }

  wr64(u, R_RTADDR, u->root_phys); // TTM = 0 (modo heredado)
  wr32(u, R_GCMD, u->gcmd | G_SRTP);
  if (wait_reg32(u, R_GSTS, 1u << 30, 1u << 30)) {
    LOG_ERR("[IOMMU] DRHD%d: RTPS no responde", u->idx);
    return -1;
  }
  unit_flush_ctx_global(u);
  unit_flush_iotlb(u, -1);

  // Interrupcion de fallos.
  if (g_fault_vector >= 0) {
    uint32_t apic;
    __asm__ volatile("cpuid"
                     : "=b"(apic)
                     : "a"(1)
                     : "ecx", "edx", "memory");
    apic >>= 24;
    wr32(u, R_FEDATA, (uint32_t)g_fault_vector);
    wr32(u, R_FEADDR, (uint32_t)(MSI_BASE_ADDR | ((uint64_t)apic << 12)));
    wr32(u, R_FEUADDR, 0);
    wr32(u, R_FECTL, 0); // IM = 0
  }
  rd32(u, R_FSTS);

  u->gcmd |= G_TE;
  wr32(u, R_GCMD, u->gcmd);
  if (wait_reg32(u, R_GSTS, G_TE, G_TE)) {
    LOG_ERR("[IOMMU] DRHD%d: TE no se activa", u->idx);
    u->gcmd &= ~G_TE;
    return -1;
  }
  u->enabled = true;
  return 0;
}

static void unit_hw_disable(iommu_unit_t *u) {
  if (!u->enabled)
    return;
  u->gcmd &= ~G_TE;
  wr32(u, R_GCMD, u->gcmd);
  wait_reg32(u, R_GSTS, G_TE, 0);
  u->enabled = false;
}

// Prepara software de una unidad: capacidades, raiz y contextos en
// pass-through. No toca el registro GCMD (eso es unit_hw_enable).
static int unit_setup(iommu_unit_t *u) {
  u->reg = (volatile uint8_t *)mmio_map(
      u->reg_phys, 0x1000, PTE_WRITABLE | PTE_NOCACHE | PTE_NX);
  if (!u->reg) {
    LOG_ERR("[IOMMU] DRHD%d: mmio_map fallo", u->idx);
    return -1;
  }
  u->cap = rd64(u, R_CAP);
  u->ecap = rd64(u, R_ECAP);
  u->iro = (uint64_t)ECAP_IRO(u->ecap) * 16;
  u->coherent = ECAP_C(u->ecap);
  // Solo se mapea una pagina de registros: los de IOTLB y de fallos deben
  // caer dentro.
  if (u->iro + 16 > 0x1000 ||
      (uint64_t)CAP_FRO(u->cap) * 16 + (uint64_t)CAP_NFR(u->cap) * 16 > 0x1000) {
    LOG_WARN("[IOMMU] DRHD%d: registros fuera de la primera pagina (cap=0x%llx "
             "ecap=0x%llx), se ignora",
             u->idx, (unsigned long long)u->cap, (unsigned long long)u->ecap);
    return -1;
  }
  unsigned sagaw = CAP_SAGAW(u->cap);
  if (sagaw & 4) {
    u->levels = 4;
    u->addr_bits = 48;
  } else if (sagaw & 2) {
    u->levels = 3;
    u->addr_bits = 39;
  } else {
    LOG_WARN("[IOMMU] DRHD%d: sin 3/4 niveles (SAGAW=0x%x), se ignora", u->idx,
             sagaw);
    return -1;
  }
  if (CAP_MGAW(u->cap) < u->addr_bits)
    u->addr_bits = CAP_MGAW(u->cap);
  if (!ECAP_PT(u->ecap)) {
    LOG_WARN("[IOMMU] DRHD%d: sin pass-through (ECAP=0x%llx), se ignora",
             u->idx, (unsigned long long)u->ecap);
    return -1;
  }
  uint32_t gsts = rd32(u, R_GSTS);
  if (gsts & G_IRE) {
    LOG_WARN("[IOMMU] DRHD%d: remapeo de interrupciones activo (firmware); no "
             "se toca esta unidad",
             u->idx);
    return -1;
  }
  unsigned nd = 1u << (4 + 2 * CAP_ND(u->cap));
  u->did_max = nd > 65536 ? 65536 : nd;
  u->did_map = (uint8_t *)kzalloc((u->did_max + 7) / 8);
  if (!u->did_map)
    return -1;
  u->did_map[0] |= 0x3; // DID 0 reservado, DID 1 compartido (pass-through)
  spin_init(&u->reg_lock);
  spin_init(&u->did_lock);
  spin_init(&u->fault_lock);
  u->root_phys = alloc_zeroed_page();
  if (!u->root_phys)
    return -1;
  LOG_INFO("[IOMMU] DRHD%d: ver=0x%x cap=0x%llx ecap=0x%llx niveles=%u "
           "dir=%ubits dominios=%u %s%s",
           u->idx, rd32(u, R_VER), (unsigned long long)u->cap,
           (unsigned long long)u->ecap, u->levels, u->addr_bits, u->did_max,
           u->coherent ? "coherente" : "no-coherente",
           CAP_CM(u->cap) ? " caching-mode" : "");
  return 0;
}

// Contextos en pass-through para todos los dispositivos de los buses que la
// unidad gobierna y en los que hay algo. Se rellena el bus entero: asi un
// dispositivo que aparezca despues (o un alias de puente) no queda bloqueado.
static void unit_fill_passthrough(iommu_unit_t *u) {
  int count = 0;
  const pci_device_t *devs = pci_get_devices(&count);
  bool bus_done[256] = {0};
  if (u->pci_all) {
    // La unidad "resto de PCI" cubre tambien lo que aparezca mas tarde
    // (hotplug, buses que Aurora no enumero): todos los buses (1 MiB).
    for (int b = 0; b < 256; b++) {
      ctx_entry_t *t = ctx_table_for_bus(u, (uint8_t)b, true);
      if (!t)
        continue;
      for (int d = 0; d < 256; d++)
        ctx_set_passthrough(u, &t[d], 1);
    }
    return;
  }
  for (int i = 0; i < count; i++) {
    uint8_t bus = devs[i].bus;
    uint16_t bdf = (uint16_t)((bus << 8) | (devs[i].slot << 3) | devs[i].func);
    if (bus_done[bus] || unit_for_bdf(bdf) != u)
      continue;
    bus_done[bus] = true;
    ctx_entry_t *t = ctx_table_for_bus(u, bus, true);
    if (!t)
      continue;
    for (int d = 0; d < 256; d++)
      ctx_set_passthrough(u, &t[d], 1);
  }
}

// ---------------------------------------------------------------------------
// Fallos.
// ---------------------------------------------------------------------------
static int unit_fault_poll(iommu_unit_t *u) {
  if (!u->enabled)
    return 0;
  unsigned long ffl = spin_lock_irqsave(&u->fault_lock);
  uint32_t fsts = rd32(u, R_FSTS);
  if (!(fsts & 0xFF)) {
    spin_unlock_irqrestore(&u->fault_lock, ffl);
    return 0;
  }
  int handled = 0;
  unsigned nfr = CAP_NFR(u->cap);
  volatile uint8_t *frcd = u->reg + CAP_FRO(u->cap) * 16;
  // Se recorren TODOS los registros (nfr es pequeno) en vez de fiarse de FRI:
  // algunos emuladores lo dejan apuntando al siguiente hueco libre.
  for (unsigned idx = 0; idx < nfr; idx++) {
    volatile uint8_t *rec = frcd + idx * 16;
    uint64_t hi = *(volatile uint64_t *)(rec + 8);
    if (!(hi & (1ULL << 63)))
      continue;
    uint64_t lo = *(volatile uint64_t *)rec;
    uint16_t sid = (uint16_t)(hi & 0xFFFF);
    uint8_t reason = (uint8_t)((hi >> 32) & 0xFF);
    bool is_read = (hi >> 62) & 1;
    *(volatile uint32_t *)(rec + 12) = 1u << 31; // limpiar F
    handled++;
    __atomic_add_fetch(&g_faults, 1, __ATOMIC_RELAXED);
    // Con g_dom_lock el dominio no se puede liberar mientras se usa.
    unsigned long dfl = spin_lock_irqsave(&g_dom_lock);
    iommu_domain_t *d = g_dom ? g_dom[sid] : NULL;
    if (d) {
      __atomic_add_fetch(&d->faults, 1, __ATOMIC_RELAXED);
      d->last_fault_iova = lo & PTE_ADDR_MASK;
      d->last_fault_reason = reason;
      if (g_fault_limit && d->faults >= g_fault_limit && !d->quarantined) {
        // Cuarentena: sin bus master el dispositivo ya no puede iniciar DMA ni
        // inundar de fallos. El acceso a la config PCI es seguro desde IRQ.
        d->quarantined = 1;
        uint8_t b = d->bdf >> 8, dv = (d->bdf >> 3) & 0x1F, fn = d->bdf & 7;
        uint16_t cmd = pci_read_config_word(b, dv, fn, 0x04);
        pci_write_config_word(b, dv, fn, 0x04, (uint16_t)(cmd & ~PCI_CMD_BUS_MASTER));
        LOG_ERR("[IOMMU] %02x:%02x.%x: %llu fallos de DMA; cuarentena (bus master "
                "desactivado)",
                b, dv, fn, (unsigned long long)d->faults);
      }
    } else {
      __atomic_add_fetch(&g_unattributed, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&g_dom_lock, dfl);
    if (g_fault_log_budget > 0) {
      g_fault_log_budget--;
      LOG_WARN("[IOMMU] DRHD%d: fallo DMA %s dev %02x:%02x.%x iova=0x%llx "
               "razon=0x%x%s",
               u->idx, is_read ? "lectura" : "escritura", sid >> 8,
               (sid >> 3) & 0x1F, sid & 7, (unsigned long long)(lo & ~0xFFFULL),
               reason, d ? "" : " (sin dominio privado)");
      if (g_fault_log_budget == 0)
        LOG_WARN("[IOMMU] demasiados fallos; se deja de registrar cada uno");
    }
  }
  wr32(u, R_FSTS, fsts & 0xFF);
  spin_unlock_irqrestore(&u->fault_lock, ffl);
  return handled;
}

int iommu_fault_poll(void) {
  int n = 0;
  for (int i = 0; i < g_nunits; i++)
    n += unit_fault_poll(&g_units[i]);
  return n;
}

static void iommu_fault_irq(void) { iommu_fault_poll(); }

// ---------------------------------------------------------------------------
// Asignador de IOVA (huecos libres ordenados).
// ---------------------------------------------------------------------------
static int iova_reserve_locked(iommu_domain_t *d, uint64_t s, uint64_t e) {
  // Quita [s, e) de la lista de huecos.
  free_range_t **pp = &d->free;
  while (*pp) {
    free_range_t *r = *pp;
    if (r->end <= s) {
      pp = &r->next;
      continue;
    }
    if (r->start >= e)
      break;
    if (r->start < s && r->end > e) { // parte en dos
      free_range_t *n = (free_range_t *)kmalloc(sizeof(*n));
      if (!n)
        return -1;
      n->start = e;
      n->end = r->end;
      n->next = r->next;
      r->end = s;
      r->next = n;
      return 0;
    }
    if (r->start >= s && r->end <= e) { // desaparece
      *pp = r->next;
      kfree(r);
      continue;
    }
    if (r->start < s)
      r->end = s;
    else
      r->start = e;
    pp = &r->next;
  }
  return 0;
}

static uint64_t iova_alloc_locked(iommu_domain_t *d, uint64_t size,
                                  uint64_t align, uint64_t max_addr) {
  uint64_t best_a = 0;
  bool found = false;
  for (free_range_t *r = d->free; r; r = r->next) {
    uint64_t top = r->end;
    if (top > max_addr + 1)
      top = max_addr + 1;
    if (top <= r->start || top - r->start < size)
      continue;
    uint64_t a = (top - size) & ~(align - 1);
    if (a < r->start)
      continue;
    if (!found || a > best_a) {
      best_a = a;
      found = true;
    }
  }
  if (!found)
    return IOMMU_ERR;
  if (iova_reserve_locked(d, best_a, best_a + size))
    return IOMMU_ERR;
  return best_a;
}

static void iova_free_locked(iommu_domain_t *d, uint64_t s, uint64_t e) {
  free_range_t **pp = &d->free;
  while (*pp && (*pp)->end < s)
    pp = &(*pp)->next;
  free_range_t *r = *pp;
  if (r && r->start <= e) { // se solapa o toca: fusionar
    if (s < r->start)
      r->start = s;
    if (e > r->end)
      r->end = e;
    while (r->next && r->next->start <= r->end) {
      free_range_t *n = r->next;
      if (n->end > r->end)
        r->end = n->end;
      r->next = n->next;
      kfree(n);
    }
    return;
  }
  free_range_t *n = (free_range_t *)kmalloc(sizeof(*n));
  if (!n)
    return; // se pierde ese hueco (mejor eso que reutilizarlo mal)
  n->start = s;
  n->end = e;
  n->next = *pp;
  *pp = n;
}

// ---------------------------------------------------------------------------
// Tablas de paginas del dominio.
// ---------------------------------------------------------------------------
static uint64_t *pt_leaf(iommu_domain_t *d, uint64_t iova, bool create) {
  uint64_t tbl = d->pgd_phys;
  for (unsigned lvl = d->levels; lvl > 1; lvl--) {
    uint64_t *t = (uint64_t *)phys_to_virt(tbl);
    unsigned idx = (unsigned)((iova >> (12 + 9 * (lvl - 1))) & 511);
    uint64_t e = t[idx];
    if (!(e & 3)) {
      if (!create)
        return NULL;
      uint64_t pg = alloc_zeroed_page();
      if (!pg)
        return NULL;
      flush_entry(d->unit, phys_to_virt(pg), 4096);
      t[idx] = pg | 3;
      flush_entry(d->unit, &t[idx], 8);
      d->tables++;
      e = t[idx];
    }
    tbl = e & PTE_ADDR_MASK;
  }
  return &((uint64_t *)phys_to_virt(tbl))[(iova >> 12) & 511];
}

static void pt_free_level(iommu_domain_t *d, uint64_t tbl, unsigned lvl) {
  uint64_t *t = (uint64_t *)phys_to_virt(tbl);
  if (lvl > 1)
    for (int i = 0; i < 512; i++)
      if (t[i] & 3)
        pt_free_level(d, t[i] & PTE_ADDR_MASK, lvl - 1);
  pmm_free_page(tbl);
}

static int dom_map_page(iommu_domain_t *d, uint64_t iova, uint64_t phys,
                        unsigned prot) {
  uint64_t *pte = pt_leaf(d, iova, true);
  if (!pte)
    return -1;
  uint64_t v = (phys & PTE_ADDR_MASK) | ((prot & IOMMU_PROT_R) ? 1 : 0) |
               ((prot & IOMMU_PROT_W) ? 2 : 0);
  *pte = v;
  flush_entry(d->unit, pte, 8);
  return 0;
}

static void dom_unmap_page(iommu_domain_t *d, uint64_t iova) {
  uint64_t *pte = pt_leaf(d, iova, false);
  if (!pte)
    return;
  *pte = 0;
  flush_entry(d->unit, pte, 8);
}

// ---------------------------------------------------------------------------
// Dominios.
// ---------------------------------------------------------------------------
static void dom_identity_rmrr(iommu_domain_t *d) {
  uint8_t bus = d->bdf >> 8, devfn = d->bdf & 0xFF;
  for (int i = 0; i < g_nrmrr; i++) {
    rmrr_t *r = &g_rmrr[i];
    bool hit = false;
    for (int k = 0; k < r->nscopes && !hit; k++)
      hit = scope_matches(&r->scopes[k], bus, devfn);
    if (!hit)
      continue;
    uint64_t s = r->base & ~0xFFFULL, e = (r->limit | 0xFFF) + 1;
    iova_reserve_locked(d, s, e);
    for (uint64_t a = s; a < e; a += 4096)
      dom_map_page(d, a, a, IOMMU_PROT_R | IOMMU_PROT_W);
    LOG_INFO("[IOMMU] %02x:%02x.%x: RMRR 0x%llx-0x%llx mapeada 1:1", bus,
             devfn >> 3, devfn & 7, (unsigned long long)r->base,
             (unsigned long long)r->limit);
  }
}

static bool device_is_igfx(uint16_t bdf) {
  uint8_t b = bdf >> 8, dv = (bdf >> 3) & 0x1F, fn = bdf & 7;
  if (pci_read_config_word(b, dv, fn, 0) != 0x8086)
    return false;
  return pci_read_config_byte(b, dv, fn, 0x0B) == PCI_CLASS_DISPLAY && b == 0;
}

iommu_domain_t *iommu_domain_find(uint16_t bdf) {
  return g_dom ? g_dom[bdf] : NULL;
}

static iommu_domain_t *iommu_domain_create_locked(iommu_unit_t *u,
                                                  uint16_t bdf);

iommu_domain_t *iommu_domain_get(uint16_t bdf) {
  if (!g_active || !g_dom)
    return NULL;
  iommu_domain_t *d = g_dom[bdf];
  if (d)
    return d;
  if (g_igfx_off && device_is_igfx(bdf))
    return NULL;
  iommu_unit_t *u = unit_for_bdf(bdf);
  if (!u || !u->enabled)
    return NULL;

  unsigned long cfl = spin_lock_irqsave(&g_create_lock);
  d = iommu_domain_create_locked(u, bdf);
  spin_unlock_irqrestore(&g_create_lock, cfl);
  return d;
}

static iommu_domain_t *iommu_domain_create_locked(iommu_unit_t *u,
                                                  uint16_t bdf) {
  iommu_domain_t *d = g_dom[bdf];
  if (d) // otro hilo gano la carrera
    return d;
  unsigned long fl;
  int did = did_alloc(u);
  if (did < 0) {
    LOG_WARN("[IOMMU] sin dominios libres para %02x:%02x.%x", bdf >> 8,
             (bdf >> 3) & 0x1F, bdf & 7);
    return NULL;
  }
  d = (iommu_domain_t *)kzalloc(sizeof(*d));
  if (!d) {
    did_free(u, did);
    return NULL;
  }
  d->unit = u;
  d->bdf = bdf;
  d->did = (uint16_t)did;
  d->levels = u->levels;
  d->limit = (u->addr_bits >= 64) ? ~0ULL : ((1ULL << u->addr_bits) - 1);
  spin_init(&d->lock);
  d->pgd_phys = alloc_zeroed_page();
  free_range_t *all = (free_range_t *)kmalloc(sizeof(*all));
  if (!d->pgd_phys || !all) {
    if (d->pgd_phys)
      pmm_free_page(d->pgd_phys);
    kfree(all);
    kfree(d);
    did_free(u, did);
    return NULL;
  }
  flush_entry(u, phys_to_virt(d->pgd_phys), 4096);
  all->start = 0x1000; // la IOVA 0 nunca se entrega
  all->end = d->limit + 1;
  all->next = NULL;
  d->free = all;
  iova_reserve_locked(d, MSI_BASE_ADDR, MSI_HOLE_END);
  dom_identity_rmrr(d);

  // Enganchar: primero la parte alta de la entrada (AW/DID) y luego la baja
  // con P y la raiz de tablas (escritura atomica de 64 bits). La entrada ya
  // estaba presente en pass-through, asi que el dispositivo no ve un hueco.
  ctx_entry_t *t = ctx_table_for_bus(u, bdf >> 8, true);
  if (!t) {
    pt_free_level(d, d->pgd_phys, d->levels);
    kfree(d);
    did_free(u, did);
    return NULL;
  }
  ctx_entry_t *e = &t[bdf & 0xFF];
  e->hi = (uint64_t)(u->levels - 2) | ((uint64_t)did << 8);
  e->lo = (d->pgd_phys & PTE_ADDR_MASK) | 1; // T=0 (traducir), P=1
  flush_entry(u, e, 16);
  unit_flush_ctx_global(u);
  unit_flush_iotlb(u, -1);

  fl = spin_lock_irqsave(&g_dom_lock);
  g_dom[bdf] = d;
  g_ndomains++;
  spin_unlock_irqrestore(&g_dom_lock, fl);
  LOG_INFO("[IOMMU] %02x:%02x.%x: dominio privado %u (%u niveles, IOVA hasta "
           "0x%llx)",
           bdf >> 8, (bdf >> 3) & 0x1F, bdf & 7, did, d->levels,
           (unsigned long long)d->limit);
  return d;
}

void iommu_domain_release(uint16_t bdf) {
  if (!g_dom)
    return;
  unsigned long cfl = spin_lock_irqsave(&g_create_lock);
  unsigned long fl = spin_lock_irqsave(&g_dom_lock);
  iommu_domain_t *d = g_dom[bdf];
  g_dom[bdf] = NULL;
  if (d)
    g_ndomains--;
  spin_unlock_irqrestore(&g_dom_lock, fl);
  if (!d) {
    spin_unlock_irqrestore(&g_create_lock, cfl);
    return;
  }
  iommu_unit_t *u = d->unit;
  ctx_entry_t *t = ctx_table_for_bus(u, bdf >> 8, false);
  if (t) {
    ctx_set_passthrough(u, &t[bdf & 0xFF], 1);
    unit_flush_ctx_global(u);
    unit_flush_iotlb(u, -1);
  }
  pt_free_level(d, d->pgd_phys, d->levels);
  while (d->free) {
    free_range_t *n = d->free->next;
    kfree(d->free);
    d->free = n;
  }
  did_free(u, d->did);
  kfree(d);
  spin_unlock_irqrestore(&g_create_lock, cfl);
}

// ---------------------------------------------------------------------------
// Mapeo.
// ---------------------------------------------------------------------------
uint64_t iommu_dma_map(iommu_domain_t *d, uint64_t phys, uint64_t size,
                       unsigned prot, uint64_t mask) {
  if (!d || !size)
    return IOMMU_ERR;
  uint64_t off = phys & 0xFFF;
  uint64_t start = phys & ~0xFFFULL;
  uint64_t bytes = (off + size + 0xFFF) & ~0xFFFULL;
  uint64_t max_addr = mask < d->limit ? mask : d->limit;

  // Alineada a la potencia de dos que cubre el tamano (hasta 2 MiB), como
  // Linux: los drivers de GPU y los dispositivos con ventanas de DMA lo
  // esperan de las asignaciones coherentes.
  uint64_t align = 4096;
  while (align < bytes && align < (2ULL << 20))
    align <<= 1;
  unsigned long fl = spin_lock_irqsave(&d->lock);
  uint64_t iova = iova_alloc_locked(d, bytes, align, max_addr);
  if (iova == IOMMU_ERR) {
    spin_unlock_irqrestore(&d->lock, fl);
    return IOMMU_ERR;
  }
  uint64_t done = 0;
  for (; done < bytes; done += 4096) {
    if (dom_map_page(d, iova + done, start + done, prot)) {
      for (uint64_t k = 0; k < done; k += 4096)
        dom_unmap_page(d, iova + k);
      iova_free_locked(d, iova, iova + bytes);
      spin_unlock_irqrestore(&d->lock, fl);
      return IOMMU_ERR;
    }
  }
  d->maps++;
  d->pages_live += bytes >> 12;
  spin_unlock_irqrestore(&d->lock, fl);
  // Los emuladores (caching mode) y algunos equipos cachean entradas "no
  // presentes": invalidar tambien al mapear.
  if (CAP_CM(d->unit->cap))
    unit_flush_iotlb(d->unit, d->did);
  else
    flush_write_buffer(d->unit);
  return iova + off;
}

void iommu_dma_unmap(iommu_domain_t *d, uint64_t iova, uint64_t size) {
  if (!d || !size)
    return;
  uint64_t off = iova & 0xFFF;
  uint64_t start = iova & ~0xFFFULL;
  uint64_t bytes = (off + size + 0xFFF) & ~0xFFFULL;
  unsigned long fl = spin_lock_irqsave(&d->lock);
  // Un unmap de algo que no esta mapeado (doble unmap, direccion inventada)
  // no debe devolver al asignador zonas reservadas (MSI, RMRR).
  uint64_t *first = pt_leaf(d, start, false);
  if (!first || !(*first & 3) ||
      (start < MSI_HOLE_END && start + bytes > MSI_BASE_ADDR)) {
    spin_unlock_irqrestore(&d->lock, fl);
    LOG_WARN("[IOMMU] %02x:%02x.%x: unmap de IOVA no mapeada 0x%llx", d->bdf >> 8,
             (d->bdf >> 3) & 0x1F, d->bdf & 7, (unsigned long long)iova);
    return;
  }
  for (uint64_t k = 0; k < bytes; k += 4096)
    dom_unmap_page(d, start + k);
  d->unmaps++;
  d->pages_live -= d->pages_live >= (bytes >> 12) ? (bytes >> 12) : d->pages_live;
  spin_unlock_irqrestore(&d->lock, fl);
  // Primero invalidar y despues reutilizar la IOVA: si no, el dispositivo
  // podria seguir usando una traduccion vieja sobre memoria ya reasignada.
  unit_flush_iotlb(d->unit, d->did);
  fl = spin_lock_irqsave(&d->lock);
  iova_free_locked(d, start, start + bytes);
  spin_unlock_irqrestore(&d->lock, fl);
}

uint64_t iommu_iova_to_phys(iommu_domain_t *d, uint64_t iova) {
  if (!d)
    return 0;
  unsigned long fl = spin_lock_irqsave(&d->lock);
  uint64_t *pte = pt_leaf(d, iova, false);
  uint64_t r = 0;
  if (pte && (*pte & 3))
    r = (*pte & PTE_ADDR_MASK) | (iova & 0xFFF);
  spin_unlock_irqrestore(&d->lock, fl);
  return r;
}

// ---------------------------------------------------------------------------
// Estadisticas.
// ---------------------------------------------------------------------------
void iommu_domain_get_stats(const iommu_domain_t *d, iommu_domain_stats_t *o) {
  memset(o, 0, sizeof(*o));
  if (!d)
    return;
  o->maps = d->maps;
  o->unmaps = d->unmaps;
  o->pages_live = d->pages_live;
  o->faults = d->faults;
  o->last_fault_iova = d->last_fault_iova;
  o->last_fault_reason = d->last_fault_reason;
  o->quarantined = d->quarantined;
  o->bdf = d->bdf;
  o->did = d->did;
}

void iommu_get_stats(iommu_stats_t *o) {
  memset(o, 0, sizeof(*o));
  for (int i = 0; i < g_nunits; i++)
    if (g_units[i].enabled)
      o->units++;
  o->domains = g_ndomains;
  o->faults = g_faults;
  o->unattributed_faults = g_unattributed;
}

bool iommu_active(void) { return g_active; }

// ---------------------------------------------------------------------------
// Inicio y suspension.
// ---------------------------------------------------------------------------
int iommu_init(void) {
  if (g_initialized)
    return g_active;
  g_initialized = true;

  char v[16];
  if (!cmdline_get("iommu", v, sizeof(v)) ||
      (strcmp(v, "on") && strcmp(v, "1") && strcmp(v, "force"))) {
    LOG_INFO("[IOMMU] desactivado (usa iommu=on para activar VT-d)");
    return 0;
  }
  if (cmdline_get("iommu.igfx", v, sizeof(v)) && !strcmp(v, "off"))
    g_igfx_off = true;
  if (cmdline_get("iommu.fault_limit", v, sizeof(v))) {
    uint64_t n = 0;
    for (const char *c = v; *c >= '0' && *c <= '9'; c++)
      n = n * 10 + (uint64_t)(*c - '0');
    g_fault_limit = n;
  }

  parse_dmar();
  if (!g_nunits) {
    LOG_INFO("[IOMMU] sin tabla DMAR: este equipo no tiene VT-d (o esta "
             "apagado en la BIOS)");
    return 0;
  }

  g_dom = (iommu_domain_t **)kzalloc(65536 * sizeof(iommu_domain_t *));
  if (!g_dom)
    return 0;
  spin_init(&g_dom_lock);
  spin_init(&g_create_lock);

  // Vector de fallos compartido por todas las unidades.
  g_fault_vector = msi_vector_alloc();
  if (g_fault_vector >= 0)
    msi_install_handler((uint8_t)g_fault_vector, iommu_fault_irq);
  else
    LOG_WARN("[IOMMU] sin vector MSI libre: los fallos solo se leen por "
             "sondeo");

  int ok = 0;
  for (int i = 0; i < g_nunits; i++) {
    iommu_unit_t *u = &g_units[i];
    if (u->segment != 0) {
      u->skip = true;
      continue;
    }
    if (unit_setup(u)) {
      u->skip = true;
      continue;
    }
    unit_fill_passthrough(u);
    flush_entry(u, phys_to_virt(u->root_phys), 4096);
  }
  // Las unidades que quedan sin tratar dejan sus dispositivos sin traducir
  // (la unidad no se activa); las demas se encienden ahora.
  for (int i = 0; i < g_nunits; i++) {
    iommu_unit_t *u = &g_units[i];
    if (u->skip)
      continue;
    if (unit_hw_enable(u) == 0) {
      ok++;
      LOG_INFO("[IOMMU] DRHD%d: traduccion activa (todos los dispositivos en "
               "pass-through hasta que un driver pida DMA aislado)",
               u->idx);
    } else {
      u->skip = true;
    }
  }
  g_active = ok > 0;
  return g_active;
}

// Con la traduccion apagada una IOVA se usaria como direccion fisica. Antes
// de apagarla, a todo dispositivo con dominio privado que siga con bus
// master (driver sin soporte de suspension) se le quita; vuelve al reanudar.
static void park_bus_masters(bool park) {
  if (!g_dom)
    return;
  unsigned long fl = spin_lock_irqsave(&g_dom_lock);
  for (unsigned bdf = 0; bdf < 65536 && g_ndomains; bdf++) {
    iommu_domain_t *d = g_dom[bdf];
    if (!d)
      continue;
    uint8_t b = bdf >> 8, dv = (bdf >> 3) & 0x1F, fn = bdf & 7;
    uint16_t cmd = pci_read_config_word(b, dv, fn, 0x04);
    if (park) {
      if (cmd != 0xFFFF && (cmd & PCI_CMD_BUS_MASTER)) {
        pci_write_config_word(b, dv, fn, 0x04,
                              (uint16_t)(cmd & ~PCI_CMD_BUS_MASTER));
        d->bm_parked = 1;
      }
    } else if (d->bm_parked) {
      d->bm_parked = 0;
      if (cmd != 0xFFFF && !d->quarantined)
        pci_write_config_word(b, dv, fn, 0x04,
                              (uint16_t)(cmd | PCI_CMD_BUS_MASTER));
    }
  }
  spin_unlock_irqrestore(&g_dom_lock, fl);
}

int iommu_suspend(void) {
  park_bus_masters(true);
  for (int i = 0; i < g_nunits; i++)
    unit_hw_disable(&g_units[i]);
  return 0;
}

int iommu_resume(void) {
  int rc = 0;
  for (int i = 0; i < g_nunits; i++) {
    iommu_unit_t *u = &g_units[i];
    if (u->skip || !u->root_phys)
      continue;
    if (unit_hw_enable(u))
      rc = -1;
  }
  // Si alguna unidad no vuelve, sus dispositivos se quedan sin bus master:
  // mejor un dispositivo parado que DMA sin traducir.
  if (!rc)
    park_bus_masters(false);
  return rc;
}
