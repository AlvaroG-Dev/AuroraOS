// tests/uc_backend.c — /dev/aurora-vm de mentira sobre Unicorn (la CPU de QEMU en
// una biblioteca) para probar aurora-vm entero sin VT-x: el bucle de main.c, los
// dispositivos, insn.h y la entrega de interrupciones, con SeaBIOS y un CD real.
//
// Se compila junto con main.c:
//   gcc -c -include tests/uc_shim.h main.c && gcc -c -I<unicorn>/include tests/uc_backend.c &&
//   gcc -o vm-uc main.o uc_backend.o -lunicorn -lpthread
//   (uc_shim.h cambia open/ioctl de main.c por las de esta prueba)
// Limitaciones: solo modo real y protegido de 32 bits en anillo 0 para las IRQ;
// una excepcion en modo protegido termina la prueba (AVM_EXIT_SHUTDOWN).
#define _GNU_SOURCE
#define AVM_UC_BACKEND
#include "../../../../../kernel/aurora_vm.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#include <sys/ioctl.h>


static int g_fake_fd = -1;
static uc_engine *uc;
static struct { int used; uint64_t gpa, size; void *host; } g_slots[AVM_MAX_SLOTS];
static int32_t g_pending = -1;
static uint32_t g_tpr;
static unsigned g_timeout_ms = 10;
static int g_flags;

// hilo de la CPU <-> hilo del VMM
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
enum { ST_IDLE, ST_RUNNING, ST_IN_HOOK };
static int g_state = ST_IDLE;
static int g_go, g_event_ready, g_reply_ready;
static struct avm_exit g_ev;
static uint32_t g_reply;
static uint64_t g_mmio_addr;
static int g_mmio_write, g_mmio_size;
static int g_mmio_hit, g_stop_req;
static unsigned long g_stat_io, g_stat_mmio, g_stat_irq, g_stat_int, g_stat_pf;

static uint64_t mono_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------
// Segmentos
// ---------------------------------------------------------------------------
static uint64_t rreg(int id) {
  uint64_t v = 0;
  uc_reg_read(uc, id, &v);
  return v;
}
static void wreg(int id, uint64_t v) { uc_reg_write(uc, id, &v); }

static int pm(void) { return rreg(UC_X86_REG_CR0) & 1; }

// Lineal -> fisica con las tablas del huesped (32 bits, PAE y modo largo). Unicorn
// solo lee y escribe memoria fisica.
static int g_walk_rw, g_walk_us, g_walk_nx, g_walk_present;
// Recorre las tablas; con set_ad marca A (y D si write) como la CPU.
static int walk(uint64_t lin, uint64_t *phys, int write, int set_ad);
static int lin2phys(uint64_t lin, uint64_t *phys) { return walk(lin, phys, 0, 0); }
// Las tablas se leen y escriben por el puntero del anfitrion, no con uc_mem_*: el
// recorrido corre dentro del gancho de TLB y uc_mem_write ahi invalida bloques y
// hace longjmp desde dentro de Unicorn (se vio como SIGSEGV en __longjmp).
static uint8_t *phys_ptr(uint64_t pa, size_t n) {
  for (int i = 0; i < AVM_MAX_SLOTS; i++)
    if (g_slots[i].used && pa >= g_slots[i].gpa && pa + n <= g_slots[i].gpa + g_slots[i].size)
      return (uint8_t *)g_slots[i].host + (pa - g_slots[i].gpa);
  return NULL;
}
static void pt_read(uint64_t pa, void *v, size_t n) {
  uint8_t *p = phys_ptr(pa, n);
  if (p)
    memcpy(v, p, n);
  else
    memset(v, 0, n);
}
static void pt_write(uint64_t pa, const void *v, size_t n) {
  uint8_t *p = phys_ptr(pa, n);
  if (p)
    memcpy(p, v, n);
}
static int walk(uint64_t lin, uint64_t *phys, int write, int set_ad) {
  g_walk_rw = g_walk_us = 1;
  g_walk_nx = 0;
  g_walk_present = 0;
  uint64_t cr0 = rreg(UC_X86_REG_CR0), cr4 = rreg(UC_X86_REG_CR4), cr3 = rreg(UC_X86_REG_CR3);
  if (!(cr0 & 0x80000000u)) {
    *phys = lin & 0xFFFFFFFFu;
    return 0;
  }
  uc_x86_msr efer = {0xC0000080u, 0};
  uc_reg_read(uc, UC_X86_REG_MSR, &efer);
  if (!(cr4 & 0x20)) { // 32 bits
    uint32_t pde = 0, pte = 0;
    uint64_t pdea = (cr3 & 0xFFFFF000u) + ((lin >> 22) & 0x3FF) * 4;
    pt_read(pdea, &pde, 4);
    if (!(pde & 1))
      return -1;
    g_walk_rw &= (pde >> 1) & 1;
    g_walk_us &= (pde >> 2) & 1;
    if ((pde & 0x80) && (cr4 & 0x10)) {
      g_walk_present = 1;
      if (set_ad) {
        uint32_t n = pde | 0x20 | (write ? 0x40 : 0);
        if (n != pde)
          pt_write(pdea, &n, 4);
      }
      *phys = (pde & 0xFFC00000u) | (lin & 0x3FFFFF);
      return 0;
    }
    if (set_ad && !(pde & 0x20)) {
      uint32_t n = pde | 0x20;
      pt_write(pdea, &n, 4);
    }
    uint64_t ptea = (pde & 0xFFFFF000u) + ((lin >> 12) & 0x3FF) * 4;
    pt_read(ptea, &pte, 4);
    if (!(pte & 1))
      return -1;
    g_walk_present = 1;
    g_walk_rw &= (pte >> 1) & 1;
    g_walk_us &= (pte >> 2) & 1;
    if (set_ad) {
      uint32_t n = pte | 0x20 | (write ? 0x40 : 0);
      if (n != pte)
        pt_write(ptea, &n, 4);
    }
    *phys = (pte & 0xFFFFF000u) | (lin & 0xFFF);
    return 0;
  }
  const uint64_t M = 0x000FFFFFFFFFF000ULL;
  uint64_t e = 0, tbl;
  int lvl;
  if (efer.value & (1ULL << 10)) {
    tbl = cr3 & M;
    lvl = 4;
  } else {
    pt_read((cr3 & 0xFFFFFFE0u) + ((lin >> 30) & 3) * 8, &e, 8);
    if (!(e & 1))
      return -1;
    tbl = e & M;
    lvl = 2;
  }
  for (; lvl >= 1; lvl--) {
    int sh = 12 + 9 * (lvl - 1);
    uint64_t ea = tbl + ((lin >> sh) & 0x1FF) * 8;
    pt_read(ea, &e, 8);
    if (!(e & 1))
      return -1;
    g_walk_rw &= (int)((e >> 1) & 1);
    g_walk_us &= (int)((e >> 2) & 1);
    g_walk_nx |= (int)(e >> 63);
    int leaf = lvl == 1 || (e & 0x80);
    if (set_ad) {
      uint64_t n = e | 0x20 | (leaf && write ? 0x40 : 0);
      if (n != e)
        pt_write(ea, &n, 8);
    }
    if (lvl > 1 && (e & 0x80)) {
      g_walk_present = 1;
      *phys = (e & M & ~((1ULL << sh) - 1)) | (lin & ((1ULL << sh) - 1));
      return 0;
    }
    tbl = e & M;
  }
  g_walk_present = 1;
  *phys = tbl | (lin & 0xFFF);
  return 0;
}
static int lin_rw(uint64_t lin, void *buf, size_t n, int write) {
  uint8_t *b = buf;
  while (n) {
    uint64_t ph;
    size_t c = 4096 - (lin & 4095);
    if (c > n)
      c = n;
    if (lin2phys(lin, &ph) < 0)
      return -1;
    if (write ? uc_mem_write(uc, ph, b, c) : uc_mem_read(uc, ph, b, c))
      return -1;
    lin += c;
    b += c;
    n -= c;
  }
  return 0;
}

static void seg_info(int id, struct avm_seg *s) {
  uint16_t sel = (uint16_t)rreg(id);
  memset(s, 0, sizeof(*s));
  s->selector = sel;
  if (!pm() || (rreg(UC_X86_REG_EFLAGS) & (1u << 17))) {
    s->base = (uint64_t)sel << 4;
    s->limit = 0xFFFF;
    s->ar = id == UC_X86_REG_CS ? 0x9B : 0x93;
    return;
  }
  uc_x86_mmr gdtr;
  uc_reg_read(uc, UC_X86_REG_GDTR, &gdtr);
  uint64_t tbase = gdtr.base;
  if (sel & 4) {
    uc_x86_mmr ldtr;
    uc_reg_read(uc, UC_X86_REG_LDTR, &ldtr);
    tbase = ldtr.base;
  }
  uint8_t d[8] = {0};
  lin_rw(tbase + (sel & ~7u), d, 8, 0);
  s->base = (uint32_t)(d[2] | d[3] << 8 | d[4] << 16 | (uint32_t)d[7] << 24);
  uint32_t lim = d[0] | d[1] << 8 | (d[6] & 0xF) << 16;
  if (d[6] & 0x80)
    lim = (lim << 12) | 0xFFF;
  s->limit = lim;
  s->ar = d[5] | ((uint32_t)(d[6] & 0xF0) << 8);
  if (!(sel & ~3u))
    s->ar = 1u << 16; // nulo
}

// ---------------------------------------------------------------------------
// Entrega de interrupciones (lo que haria la CPU)
// ---------------------------------------------------------------------------
static int deliver_err(int vec, int has_err, uint32_t err);
static int deliver(int vec, int is_soft) { (void)is_soft; return deliver_err(vec, 0, 0); }
static int deliver_err(int vec, int has_err, uint32_t errc) {
  uint64_t fl = rreg(UC_X86_REG_EFLAGS);
  uint16_t cs = (uint16_t)rreg(UC_X86_REG_CS);
  uint32_t eip = (uint32_t)rreg(UC_X86_REG_EIP);
  if (!pm()) {
    uint16_t sp = (uint16_t)rreg(UC_X86_REG_SP), ss = (uint16_t)rreg(UC_X86_REG_SS);
    sp -= 6;
    uint16_t fr[3] = {(uint16_t)eip, cs, (uint16_t)fl};
    uc_mem_write(uc, (uint64_t)ss * 16 + sp, fr, 6);
    wreg(UC_X86_REG_SP, sp);
    uint16_t iv[2];
    uc_mem_read(uc, (uint64_t)vec * 4, iv, 4);
    fl &= ~((1u << 9) | (1u << 8) | (1u << 18));
    wreg(UC_X86_REG_EFLAGS, fl);
    wreg(UC_X86_REG_CS, iv[1]);
    wreg(UC_X86_REG_EIP, iv[0]);
    return 0;
  }
  int v86 = (fl >> 17) & 1; // V86 (ReactOS llama asi a la BIOS de video)
  uc_x86_mmr idtr;
  uc_reg_read(uc, UC_X86_REG_IDTR, &idtr);
  uint8_t g[8];
  if ((uint32_t)vec * 8 + 7 > idtr.limit || lin_rw(idtr.base + (uint64_t)vec * 8, g, 8, 0))
    return -1;
  uint32_t off = g[0] | g[1] << 8 | (uint32_t)g[6] << 16 | (uint32_t)g[7] << 24;
  uint16_t nsel = (uint16_t)(g[2] | g[3] << 8);
  int type = g[5] & 0xF;
  if (!(g[5] & 0x80) || (type != 0xE && type != 0xF))
    return -1;
  struct avm_seg ss;
  uint32_t fr[10];
  int n = 0;
  uint32_t esp;
  if (v86 && (nsel & 3))
    return -1;
  if ((v86 || (cs & 3)) && !(nsel & 3)) { // de anillo 3 (o V86) a 0: pila del TSS
    uc_x86_mmr tr;
    uc_reg_read(uc, UC_X86_REG_TR, &tr);
    uint32_t t[2];
    if (lin_rw(tr.base + 4, t, 8, 0))
      return -1;
    uint32_t oss = (uint32_t)rreg(UC_X86_REG_SS), oesp = (uint32_t)rreg(UC_X86_REG_ESP);
    uint32_t ogs = (uint32_t)rreg(UC_X86_REG_GS), ofs = (uint32_t)rreg(UC_X86_REG_FS);
    uint32_t ods = (uint32_t)rreg(UC_X86_REG_DS), oes = (uint32_t)rreg(UC_X86_REG_ES);
    if (v86) { // se sale de V86 antes de tocar segmentos
      wreg(UC_X86_REG_EFLAGS, fl & ~(1ULL << 17));
      fr[n++] = ogs;
      fr[n++] = ofs;
      fr[n++] = ods;
      fr[n++] = oes;
    }
    // Unicorn no deja cargar SS con RPL != CPL y el CPL sale del DPL de SS: se baja
    // a 0 cargando SS un instante sin PE (sin comprobaciones) y luego en serio.
    uint64_t cr0 = rreg(UC_X86_REG_CR0);
    wreg(UC_X86_REG_CR0, cr0 & ~0x80000001ULL);
    wreg(UC_X86_REG_SS, t[1] & 0xFFFF);
    wreg(UC_X86_REG_CR0, cr0);
    wreg(UC_X86_REG_SS, t[1] & 0xFFFF);
    esp = t[0];
    fr[n++] = oss; // orden de empuje: SS, ESP, EFLAGS, CS, EIP, error
    fr[n++] = oesp;
  } else {
    esp = (uint32_t)rreg(UC_X86_REG_ESP);
  }
  seg_info(UC_X86_REG_SS, &ss);
  fr[n++] = (uint32_t)fl;
  fr[n++] = cs;
  fr[n++] = eip;
  if (has_err)
    fr[n++] = errc;
  // fr esta en orden de empuje: en memoria va al reves (error en la cima)
  uint32_t mem[10];
  for (int i = 0; i < n; i++)
    mem[i] = fr[n - 1 - i];
  esp -= (uint32_t)n * 4;
  if (lin_rw(ss.base + esp, mem, (size_t)n * 4, 1))
    return -1;
  wreg(UC_X86_REG_ESP, esp);
  fl &= ~((1u << 8) | (1u << 14) | (1u << 16) | (1u << 17));
  if (type == 0xE)
    fl &= ~(1u << 9);
  wreg(UC_X86_REG_EFLAGS, fl);
  if (v86) {
    wreg(UC_X86_REG_DS, 0); wreg(UC_X86_REG_ES, 0);
    wreg(UC_X86_REG_FS, 0); wreg(UC_X86_REG_GS, 0);
  }
  wreg(UC_X86_REG_CS, nsel);
  wreg(UC_X86_REG_EIP, off);
  if ((uint16_t)rreg(UC_X86_REG_CS) != nsel) {
    static int warned;
    if (!warned++)
      fprintf(stderr, "[uc] no se pudo cargar CS=%x (queda %x) al entregar el vector %d\n", nsel,
              (unsigned)rreg(UC_X86_REG_CS), vec);
    return -1;
  }
  return 0;
}

static int g_shutdown;
static void pc_dump(void);
static uint32_t g_int_log[32][3];
static unsigned g_int_n;
static void intr_hook(uc_engine *u, uint32_t intno, void *ud) {
  (void)u; (void)ud;
  g_stat_int++;
  g_int_log[g_int_n & 31][0] = intno;
  g_int_log[g_int_n & 31][1] = (uint32_t)rreg(UC_X86_REG_CS);
  g_int_log[g_int_n & 31][2] = (uint32_t)rreg(UC_X86_REG_EIP);
  g_int_n++;
  if (!pm()) { // modo real (BIOS): por la IVT
    deliver((int)intno, 1);
    return;
  }
  // Modo protegido. INT n (CD nn / CC / CE detras de EIP): si el DPL de la puerta es
  // menor que el CPL (anillo 3 o V86) la CPU da #GP(n*8+2): es como ReactOS/Windows
  // llevan las llamadas a la BIOS de video en V86. Excepciones con codigo de error:
  // Unicorn no lo da; se entrega 0 (lo habitual en #GP de V86 y anillo 3).
  int rc = -1;
  uint64_t fl = rreg(UC_X86_REG_EFLAGS);
  int cpl = (fl & (1u << 17)) ? 3 : (int)(rreg(UC_X86_REG_CS) & 3);
  struct avm_seg c;
  seg_info(UC_X86_REG_CS, &c);
  uint32_t eip = (uint32_t)rreg(UC_X86_REG_EIP);
  uint8_t ib[2] = {0, 0};
  lin_rw(c.base + eip - 2, ib, 2, 0);
  int soft = (ib[0] == 0xCD && ib[1] == intno) || (intno == 3 && ib[1] == 0xCC) ||
             (intno == 4 && ib[1] == 0xCE);
  if (soft) {
    uc_x86_mmr idtr;
    uc_reg_read(uc, UC_X86_REG_IDTR, &idtr);
    uint8_t g[8] = {0};
    lin_rw(idtr.base + (uint64_t)intno * 8, g, 8, 0);
    int dpl = (g[5] >> 5) & 3;
    if (dpl < cpl) {
      wreg(UC_X86_REG_EIP, eip - (ib[0] == 0xCD ? 2 : 1));
      rc = deliver_err(13, 1, intno * 8 + 2);
    } else {
      rc = deliver((int)intno, 1);
    }
  } else {
    int errc = intno == 8 || (intno >= 10 && intno <= 14) || intno == 17;
    rc = errc ? deliver_err((int)intno, 1, 0) : deliver((int)intno, 1);
  }
  if (rc < 0) {
    pc_dump();
    fprintf(stderr, "[uc] excepcion/INT 0x%x en modo protegido en cs=%x eip=%x: no se pudo entregar\n",
            intno, (unsigned)rreg(UC_X86_REG_CS), (unsigned)rreg(UC_X86_REG_EIP));
    g_shutdown = 1;
    uc_emu_stop(uc);
  }
}

// ---------------------------------------------------------------------------
// E/S: el hilo de la CPU espera la respuesta del VMM (como pending_in del kernel)
// ---------------------------------------------------------------------------
static uint32_t io_exit(uint32_t port, int size, int write, uint32_t v) {
  pthread_mutex_lock(&mu);
  memset(&g_ev, 0, sizeof(g_ev));
  g_ev.reason = AVM_EXIT_IO;
  g_ev.io.port = (uint16_t)port;
  g_ev.io.size = (uint8_t)size;
  g_ev.io.is_write = (uint8_t)write;
  g_ev.io.count = 1;
  g_ev.io.data = v;
  g_state = ST_IN_HOOK;
  g_event_ready = 1;
  pthread_cond_broadcast(&cv);
  while (!g_reply_ready)
    pthread_cond_wait(&cv, &mu);
  g_reply_ready = 0;
  g_state = ST_RUNNING;
  uint32_t r = g_reply;
  // No se para aqui aunque haya una IRQ pendiente: uc_emu_stop dentro de un REP INS
  // aborta la iteracion y la repite (el puerto ya se leyo: se perderia un dato y,
  // con IF=0 y la IRQ esperando, el REP no terminaria nunca). La IRQ entra en la
  // siguiente vuelta (el tiempo maximo de cada RUN es de 10 ms).
  pthread_mutex_unlock(&mu);
  g_stat_io++;
  return r;
}
static uint32_t in_hook(uc_engine *u, uint32_t port, int size, void *ud) {
  (void)u; (void)ud;
  return io_exit(port, size, 0, 0);
}
static void out_hook(uc_engine *u, uint32_t port, int size, uint32_t v, void *ud) {
  (void)u; (void)ud;
  io_exit(port, size, 1, v);
}
// Unicorn no siempre deja EIP en la instruccion que falla (en codigo de 64 bits se
// queda al principio del bloque): se apunta la direccion de cada instruccion.
static uint64_t g_last_pc, g_pc_ring[64];
static uint32_t g_last_size;
static unsigned g_pc_n;
static uint64_t g_expect_pc = ~0ULL;
// Fin de cada RUN: se para entre instrucciones desde aqui y no con el tiempo maximo
// de uc_emu_start: ese para en mitad de un REP INSW, Unicorn repite la iteracion y
// el dato ya leido del puerto se pierde (se vio en el IDENTIFY: faltaba una palabra).
static uint64_t g_run_deadline_us;
static unsigned g_insn_tick;
static uint64_t mono_us(void);
static void code_hook(uc_engine *u, uint64_t a, uint32_t size, void *ud) {
  (void)ud;
  if ((++g_insn_tick & 1023) == 0 && mono_us() > g_run_deadline_us)
    uc_emu_stop(u);
  if (g_expect_pc != ~0ULL) {
    if (a != g_expect_pc)
      fprintf(stderr, "[uc] arranque en %llx, se esperaba %llx (cs=%x eip=%x)\n", (unsigned long long)a,
              (unsigned long long)g_expect_pc, (unsigned)rreg(UC_X86_REG_CS), (unsigned)rreg(UC_X86_REG_EIP));
    g_expect_pc = ~0ULL;
  }
  if (a != g_last_pc + g_last_size) // destino de un salto: se apunta (y el origen)
    g_pc_ring[g_pc_n++ & 63] = (g_last_pc << 32) | (a & 0xFFFFFFFFu);
  g_last_pc = a;
  g_last_size = size;
}
static void pc_dump(void) {
  fprintf(stderr, "[uc] ultimas instrucciones:");
  for (unsigned i = g_pc_n > 64 ? g_pc_n - 64 : 0; i < g_pc_n; i++)
    fprintf(stderr, " %llx->%llx", (unsigned long long)(g_pc_ring[i & 63] >> 32),
            (unsigned long long)(g_pc_ring[i & 63] & 0xFFFFFFFFu));
  fprintf(stderr, "\n");
}
// Unicorn (UC_TLB_CPU) se traga los fallos de pagina y repite la instruccion para
// siempre: con UC_TLB_VIRTUAL la traduccion la hace este gancho y, si falla, se
// entrega #PF por la IDT como haria la CPU.
static int g_pf, g_pf_err, g_until_fill;
static void dbg_tlb(uint64_t va, int t, int ok);
static uint64_t g_pf_addr;
static bool tlb_hook(uc_engine *u, uint64_t va, uc_mem_type t, uc_tlb_entry *e, void *ud) {
  (void)u; (void)ud;
  if (va >= 0xFFFFFFFFFFFFF000ULL) { // al parar (HLT...) Unicorn mira la direccion "until" (~0)
    g_until_fill = 1;
    return false;
  }
  int write = t == UC_MEM_WRITE, fetch = t == UC_MEM_FETCH;
  uint64_t ph;
  int user = (rreg(UC_X86_REG_CS) & 3) == 3;
  int r = walk(va, &ph, write, 1);
  uint64_t cr0 = rreg(UC_X86_REG_CR0);
  // U/S no se comprueba: la CPU lee la GDT/IDT/TSS como supervisor aunque CPL sea 3
  // y el gancho no distingue esos accesos implicitos (un #PF ahi rompe Unicorn).
  int bad = r < 0 || (write && !g_walk_rw && (user || (cr0 & 0x10000)));
  dbg_tlb(va, (int)t, !bad);
  if (bad) {
    if (getenv("UCPF")) fprintf(stderr, "[uc] tlb fallo va=%llx t=%d r=%d\n", (unsigned long long)va, t, r);
    g_pf = 1;
    g_pf_addr = va;
    g_pf_err = (g_walk_present ? 1 : 0) | (write ? 2 : 0) | (user ? 4 : 0) | (fetch && g_walk_nx ? 16 : 0);
    return false;
  }
  e->paddr = ph & ~0xFFFULL;
  e->perms = UC_PROT_READ | UC_PROT_EXEC | (g_walk_rw || !(cr0 & 0x10000) ? UC_PROT_WRITE : 0);
  return true;
}
static bool unmapped_hook(uc_engine *u, uc_mem_type t, uint64_t a, int size, int64_t v, void *ud) {
  if (getenv("UCPF")) fprintf(stderr, "[uc] unmapped a=%llx t=%d\n", (unsigned long long)a, t);
  (void)u; (void)v; (void)ud;
  g_mmio_addr = a;
  g_mmio_size = size;
  g_mmio_write = t == UC_MEM_WRITE_UNMAPPED;
  g_mmio_hit = t == UC_MEM_FETCH_UNMAPPED ? 2 : 1;
  return false;
}

// UC_MODE_32: uc_emu_start pone EIP = begin tal cual (en UC_MODE_16 lo recorta a
// 16 bits y la BIOS en modo protegido, por encima de 64 KiB, saltaria a otro sitio)
static uint64_t pc_begin(void) { return rreg(UC_X86_REG_EIP); }

static void *cpu_thread(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!g_go)
      pthread_cond_wait(&cv, &mu);
    g_go = 0;
    g_state = ST_RUNNING;
    g_stop_req = 0;
    // IRQ pendiente: entrar con IF=1 (sin sombra de STI: aproximacion)
    if (g_pending >= 0 && (rreg(UC_X86_REG_EFLAGS) & (1u << 9))) {
      int tpr_ok = !(g_pending & AVM_IRQ_TPR) || ((uint32_t)g_pending & 0xF0) > (g_tpr & 0xF0);
      if (tpr_ok && deliver(g_pending & 0xFF, 0) == 0) {
        g_pending = -1;
        g_stat_irq++;
      }
    }
    unsigned tmo = g_timeout_ms ? g_timeout_ms : 10;
    pthread_mutex_unlock(&mu);
    g_mmio_hit = 0;
    uint64_t t0 = mono_us();
    {
      struct avm_seg c;
      seg_info(UC_X86_REG_CS, &c);
      g_expect_pc = c.base + rreg(UC_X86_REG_EIP); // comprobacion: el CPU arranca donde debe
    }
    g_run_deadline_us = t0 + (uint64_t)tmo * 1000;
    int err;
    for (;;) {
      g_pf = 0;
      g_until_fill = 0;
      err = uc_emu_start(uc, pc_begin(), ~0ULL, 0, 0);
      if (err == UC_ERR_EXCEPTION && g_until_fill && !g_pf && !g_mmio_hit)
        err = UC_ERR_OK;
      if (!g_pf || g_shutdown || g_mmio_hit)
        break;
      // fallo de pagina: EIP en la instruccion, CR2 y codigo de error
      struct avm_seg c;
      seg_info(UC_X86_REG_CS, &c);
      wreg(UC_X86_REG_EIP, (uint32_t)(g_last_pc - c.base));
      {
        uint64_t cr3 = rreg(UC_X86_REG_CR3);
        wreg(UC_X86_REG_CR2, g_pf_addr); // Unicorn 2.1.1 lo escribe tambien en CR3
        if (rreg(UC_X86_REG_CR3) != cr3)
          wreg(UC_X86_REG_CR3, cr3);
      }
      g_stat_pf++;
      if (deliver_err(14, 1, (uint32_t)g_pf_err) < 0) {
        fprintf(stderr, "[uc] no se pudo entregar #PF en %llx\n", (unsigned long long)g_pf_addr);
        g_shutdown = 1;
        break;
      }
      g_mmio_hit = 0;
      if (mono_us() > g_run_deadline_us) {
        err = UC_ERR_OK;
        break;
      }
    }
    uint64_t dt = mono_us() - t0;
    pthread_mutex_lock(&mu);
    memset(&g_ev, 0, sizeof(g_ev));
    if (g_shutdown) {
      g_ev.reason = AVM_EXIT_SHUTDOWN;
    } else if (g_mmio_hit == 1 && (err == UC_ERR_READ_UNMAPPED || err == UC_ERR_WRITE_UNMAPPED ||
                                   err == UC_ERR_EXCEPTION)) { // EXCEPTION: con UC_TLB_VIRTUAL
      g_ev.reason = AVM_EXIT_MMIO;
      g_ev.mmio.gpa = g_mmio_addr; // el gancho ya da la direccion fisica
      g_ev.mmio.size = (uint8_t)g_mmio_size;
      g_ev.mmio.is_write = (uint8_t)g_mmio_write;
      g_stat_mmio++;
      {
        struct avm_seg c;
        seg_info(UC_X86_REG_CS, &c);
        wreg(UC_X86_REG_EIP, (uint32_t)(g_last_pc - c.base));
      }
    } else if (err != UC_ERR_OK) {
      pc_dump();
      fprintf(stderr, "[uc] uc_emu_start: %s (cs=%x eip=%x)\n", uc_strerror(err),
              (unsigned)rreg(UC_X86_REG_CS), (unsigned)rreg(UC_X86_REG_EIP));
      g_ev.reason = AVM_EXIT_SHUTDOWN;
    } else {
      // HLT: Unicorn para con la CPU detenida y EIP detras del HLT
      uint8_t b = 0;
      struct avm_seg cs;
      seg_info(UC_X86_REG_CS, &cs);
      lin_rw(cs.base + rreg(UC_X86_REG_EIP) - 1, &b, 1, 0);
      if (b == 0xF4 && dt < (uint64_t)tmo * 1000 * 9 / 10)
        g_ev.reason = AVM_EXIT_HLT;
      else
        g_ev.reason = AVM_EXIT_SIGNAL;
    }
    g_state = ST_IDLE;
    g_event_ready = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// ioctl
// ---------------------------------------------------------------------------
static int vm_run(struct avm_exit *x) {
  pthread_mutex_lock(&mu);
  if (g_state == ST_IN_HOOK) {
    g_reply = x->io.data;
    g_reply_ready = 1;
  } else {
    g_go = 1;
  }
  pthread_cond_broadcast(&cv);
  while (!g_event_ready)
    pthread_cond_wait(&cv, &mu);
  g_event_ready = 0;
  *x = g_ev;
  x->irq_pending = g_pending;
  x->tpr = g_tpr;
  pthread_mutex_unlock(&mu);
  return 0;
}

static void get_regs(struct avm_regs *r) {
  memset(r, 0, sizeof(*r));
  r->rax = rreg(UC_X86_REG_EAX); r->rbx = rreg(UC_X86_REG_EBX);
  r->rcx = rreg(UC_X86_REG_ECX); r->rdx = rreg(UC_X86_REG_EDX);
  r->rsi = rreg(UC_X86_REG_ESI); r->rdi = rreg(UC_X86_REG_EDI);
  r->rbp = rreg(UC_X86_REG_EBP); r->rsp = rreg(UC_X86_REG_ESP);
  r->rip = rreg(UC_X86_REG_EIP); r->rflags = rreg(UC_X86_REG_EFLAGS);
  r->cr0 = rreg(UC_X86_REG_CR0); r->cr2 = rreg(UC_X86_REG_CR2);
  r->cr3 = rreg(UC_X86_REG_CR3); r->cr4 = rreg(UC_X86_REG_CR4);
  uc_x86_msr efer = {0xC0000080u, 0};
  uc_reg_read(uc, UC_X86_REG_MSR, &efer);
  r->efer = efer.value;
  // UC_MODE_32 solo deja leer los registros de 32 bits: en modo largo la mitad
  // alta se pierde (los huespedes de prueba viven por debajo de 4 GiB)
  seg_info(UC_X86_REG_CS, &r->cs); seg_info(UC_X86_REG_DS, &r->ds);
  seg_info(UC_X86_REG_ES, &r->es); seg_info(UC_X86_REG_FS, &r->fs);
  seg_info(UC_X86_REG_GS, &r->gs); seg_info(UC_X86_REG_SS, &r->ss);
  uc_x86_mmr m;
  uc_reg_read(uc, UC_X86_REG_GDTR, &m); r->gdt_base = m.base; r->gdt_limit = m.limit;
  uc_reg_read(uc, UC_X86_REG_IDTR, &m); r->idt_base = m.base; r->idt_limit = m.limit;
}

static int g_regs_set_once;
static void set_regs(const struct avm_regs *r) {
  wreg(UC_X86_REG_EAX, r->rax & 0xFFFFFFFF); wreg(UC_X86_REG_EBX, r->rbx & 0xFFFFFFFF);
  wreg(UC_X86_REG_ECX, r->rcx & 0xFFFFFFFF); wreg(UC_X86_REG_EDX, r->rdx & 0xFFFFFFFF);
  wreg(UC_X86_REG_ESI, r->rsi & 0xFFFFFFFF); wreg(UC_X86_REG_EDI, r->rdi & 0xFFFFFFFF);
  wreg(UC_X86_REG_EBP, r->rbp & 0xFFFFFFFF); wreg(UC_X86_REG_ESP, r->rsp & 0xFFFFFFFF);
  wreg(UC_X86_REG_EFLAGS, r->rflags);
  if (g_regs_set_once && pm() && !(r->cr0 & 1)) { // reinicio del huesped: vuelta a modo real
    wreg(UC_X86_REG_CR0, r->cr0);
    wreg(UC_X86_REG_CR3, 0);
    wreg(UC_X86_REG_CR4, 0);
    g_regs_set_once = 0;
  }
  if (!g_regs_set_once || !pm()) { // estado inicial (o reinicio): segmentos en modo real
    if (r->cr0 & 1) {
      fprintf(stderr, "[uc] SET_REGS en modo protegido no soportado\n");
    } else {
      wreg(UC_X86_REG_CR0, r->cr0); // UC_MODE_32 arranca con PE=1
      wreg(UC_X86_REG_CS, r->cs.selector); wreg(UC_X86_REG_DS, r->ds.selector);
      wreg(UC_X86_REG_ES, r->es.selector); wreg(UC_X86_REG_SS, r->ss.selector);
      wreg(UC_X86_REG_FS, r->fs.selector); wreg(UC_X86_REG_GS, r->gs.selector);
    }
  }
  wreg(UC_X86_REG_EIP, r->rip & 0xFFFFFFFF);
  g_regs_set_once = 1;
}

static int set_memory(const struct avm_memory *m) {
  if (m->slot >= AVM_MAX_SLOTS)
    return -EINVAL;
  if (g_slots[m->slot].used) {
    uc_mem_unmap(uc, g_slots[m->slot].gpa, g_slots[m->slot].size);
    munmap(g_slots[m->slot].host, g_slots[m->slot].size);
    g_slots[m->slot].used = 0;
  }
  if (!m->size)
    return 0;
  void *p = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, (off_t)m->offset);
  if (p == MAP_FAILED)
    return -errno;
  int e = uc_mem_map_ptr(uc, m->gpa, m->size, UC_PROT_ALL, p);
  if (e) {
    fprintf(stderr, "[uc] uc_mem_map_ptr %llx+%llx: %s\n", (unsigned long long)m->gpa,
            (unsigned long long)m->size, uc_strerror(e));
    munmap(p, m->size);
    return -EEXIST;
  }
  g_slots[m->slot].used = 1;
  g_slots[m->slot].gpa = m->gpa;
  g_slots[m->slot].size = m->size;
  g_slots[m->slot].host = p;
  return 0;
}

static pthread_t g_thread;
static void uc_init(void) {
  if (uc) { // VM nueva: CPU y memoria desde cero
    uc_close(uc);
    for (int i = 0; i < AVM_MAX_SLOTS; i++)
      if (g_slots[i].used) {
        munmap(g_slots[i].host, g_slots[i].size);
        g_slots[i].used = 0;
      }
    g_pending = -1;
    g_tpr = 0;
    g_shutdown = 0;
    g_regs_set_once = 0;
    g_state = ST_IDLE;
  }
  if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc)) {
    fprintf(stderr, "[uc] uc_open fallo\n");
    exit(1);
  }
  // como una CPU recien encendida: modo real (UC_MODE_32 empieza con PE=1)
  wreg(UC_X86_REG_CR0, 0x10);
  for (int sr = 0; sr < 6; sr++)
    wreg((int[]){UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_FS, UC_X86_REG_GS}[sr], 0);
  uc_hook h;
  uc_hook_add(uc, &h, UC_HOOK_INSN, (void *)in_hook, NULL, 1, 0, UC_X86_INS_IN);
  uc_hook_add(uc, &h, UC_HOOK_INSN, (void *)out_hook, NULL, 1, 0, UC_X86_INS_OUT);
  uc_hook_add(uc, &h, UC_HOOK_INTR, (void *)intr_hook, NULL, 1, 0);
  uc_hook_add(uc, &h, UC_HOOK_MEM_UNMAPPED, (void *)unmapped_hook, NULL, 1, 0);
  uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
  uc_hook_add(uc, &h, UC_HOOK_TLB_FILL, (void *)tlb_hook, NULL, 1, 0);
  uc_hook_add(uc, &h, UC_HOOK_CODE, (void *)code_hook, NULL, 1, 0);
  if (!g_thread)
    pthread_create(&g_thread, NULL, cpu_thread, NULL);
}

static uint64_t g_dbg_va[16];
static int g_dbg_t[16], g_dbg_ok[16];
static unsigned g_dbg_n;
static void segv(int sig) {
  (void)sig;
  fprintf(stderr, "[uc] SIGSEGV; #PF entregados %lu; ultimas traducciones:", g_stat_pf);
  for (unsigned i = g_dbg_n > 16 ? g_dbg_n - 16 : 0; i < g_dbg_n; i++)
    fprintf(stderr, " %llx/%d/%d", (unsigned long long)g_dbg_va[i & 15], g_dbg_t[i & 15], g_dbg_ok[i & 15]);
  fprintf(stderr, "\n");
  pc_dump();
  _exit(139);
}
static void stats(void) {
  fprintf(stderr, "[uc] E/S %lu, MMIO %lu, IRQ entregadas %lu, INT n %lu, #PF %lu\n", g_stat_io,
          g_stat_mmio, g_stat_irq, g_stat_int, g_stat_pf);
  fprintf(stderr, "[uc] instrucciones/bloques vistos: %u\n", g_insn_tick);
  fprintf(stderr, "[uc] ultimas INT/excepciones (vector cs:eip):");
  for (unsigned i = g_int_n > 32 ? g_int_n - 32 : 0; i < g_int_n; i++)
    fprintf(stderr, " %x@%x:%x", g_int_log[i & 31][0], g_int_log[i & 31][1], g_int_log[i & 31][2]);
  fprintf(stderr, "\n");
  pc_dump();
}

int avm_test_open(const char *path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  int mode = va_arg(ap, int);
  va_end(ap);
  if (!strcmp(path, "/dev/aurora-vm")) {
    g_fake_fd = open("/dev/null", O_RDWR | O_CLOEXEC, 0);
    if (!uc) {
      atexit(stats);
      signal(SIGSEGV, segv);
    }
    uc_init();
    return g_fake_fd;
  }
  return open(path, flags, mode);
}

int avm_test_ioctl(int fd, unsigned long req, ...) {
  va_list ap;
  va_start(ap, req);
  unsigned long arg = va_arg(ap, unsigned long);
  va_end(ap);
  if (fd != g_fake_fd || fd < 0)
    return ioctl(fd, req, arg);
  int rc = 0;
  switch (req) {
  case AVM_GET_CAPS: {
    struct avm_caps *c = (struct avm_caps *)arg;
    memset(c, 0, sizeof(*c));
    c->flags = AVM_CAP_VMX | AVM_CAP_EPT | AVM_CAP_UNRESTRICTED | AVM_CAP_USABLE;
    break;
  }
  case AVM_SET_MEMORY: rc = set_memory((const struct avm_memory *)arg); break;
  case AVM_GET_REGS: get_regs((struct avm_regs *)arg); break;
  case AVM_SET_REGS: set_regs((const struct avm_regs *)arg); break;
  case AVM_RUN: rc = vm_run((struct avm_exit *)arg); break;
  case AVM_INJECT_IRQ:
    if (g_pending >= 0)
      rc = -EBUSY;
    else
      g_pending = (int32_t)arg;
    break;
  case AVM_WITHDRAW_IRQ: rc = g_pending; g_pending = -1; return rc;
  case AVM_SET_TPR: g_tpr = (uint32_t)arg & 0xFF; break;
  case AVM_SET_TPR_WAIT: break;
  case AVM_SET_TIMEOUT: g_timeout_ms = (unsigned)arg; break;
  case AVM_SET_DEBUG: break;
  case AVM_SET_FLAGS: g_flags = (int)arg; break;
  default: rc = -ENOTTY;
  }
  if (rc < 0) {
    errno = -rc;
    return -1;
  }
  return rc;
}

static void dbg_tlb(uint64_t va, int t, int ok) {
  g_dbg_va[g_dbg_n & 15] = va;
  g_dbg_t[g_dbg_n & 15] = t;
  g_dbg_ok[g_dbg_n & 15] = ok;
  g_dbg_n++;
}
