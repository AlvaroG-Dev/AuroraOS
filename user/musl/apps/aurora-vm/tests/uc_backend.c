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
static unsigned long g_stat_io, g_stat_mmio, g_stat_irq, g_stat_int;

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
  uc_mem_read(uc, tbase + (sel & ~7u), d, 8);
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
static int deliver(int vec, int is_soft) {
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
  if ((cs & 3) || (fl & (1u << 17)))
    return -1; // anillo 3 / V86: no lo hace esta prueba
  uc_x86_mmr idtr;
  uc_reg_read(uc, UC_X86_REG_IDTR, &idtr);
  uint8_t g[8];
  if ((uint32_t)vec * 8 + 7 > idtr.limit || uc_mem_read(uc, idtr.base + (uint64_t)vec * 8, g, 8))
    return -1;
  uint32_t off = g[0] | g[1] << 8 | (uint32_t)g[6] << 16 | (uint32_t)g[7] << 24;
  uint16_t nsel = (uint16_t)(g[2] | g[3] << 8);
  int type = g[5] & 0xF;
  if (!(g[5] & 0x80) || (type != 0xE && type != 0xF))
    return -1;
  struct avm_seg ss;
  seg_info(UC_X86_REG_SS, &ss);
  uint32_t esp = (uint32_t)rreg(UC_X86_REG_ESP) - 12;
  uint32_t fr[3] = {eip, cs, (uint32_t)fl};
  uc_mem_write(uc, ss.base + esp, fr, 12);
  wreg(UC_X86_REG_ESP, esp);
  fl &= ~((1u << 8) | (1u << 14) | (1u << 16) | (1u << 17));
  if (type == 0xE)
    fl &= ~(1u << 9);
  wreg(UC_X86_REG_EFLAGS, fl);
  wreg(UC_X86_REG_CS, nsel);
  wreg(UC_X86_REG_EIP, off);
  (void)is_soft;
  return 0;
}

static int g_shutdown;
static void pc_dump(void);
static void intr_hook(uc_engine *u, uint32_t intno, void *ud) {
  (void)u; (void)ud;
  g_stat_int++;
  // INT n en modo real (BIOS): por la IVT. Excepciones en modo protegido: fin.
  if (pm()) {
    pc_dump();
    fprintf(stderr, "[uc] excepcion/INT 0x%x en modo protegido en cs=%x eip=%x: fin de la prueba\n",
            intno, (unsigned)rreg(UC_X86_REG_CS), (unsigned)rreg(UC_X86_REG_EIP));
    g_shutdown = 1;
    uc_emu_stop(uc);
    return;
  }
  deliver((int)intno, 1);
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
  int stop = g_pending >= 0 || g_stop_req;
  pthread_mutex_unlock(&mu);
  if (stop) // hay una IRQ que entregar: parar al final del bloque
    uc_emu_stop(uc);
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
static void code_hook(uc_engine *u, uint64_t a, uint32_t size, void *ud) {
  (void)u; (void)ud;
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
static bool unmapped_hook(uc_engine *u, uc_mem_type t, uint64_t a, int size, int64_t v, void *ud) {
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
    int err = uc_emu_start(uc, pc_begin(), ~0ULL, (uint64_t)tmo * 1000, 0);
    uint64_t dt = mono_us() - t0;
    pthread_mutex_lock(&mu);
    memset(&g_ev, 0, sizeof(g_ev));
    if (g_shutdown) {
      g_ev.reason = AVM_EXIT_SHUTDOWN;
    } else if (g_mmio_hit == 1 && (err == UC_ERR_READ_UNMAPPED || err == UC_ERR_WRITE_UNMAPPED)) {
      g_ev.reason = AVM_EXIT_MMIO;
      g_ev.mmio.gpa = g_mmio_addr; // sin paginacion: lineal = fisica
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
      uc_mem_read(uc, cs.base + rreg(UC_X86_REG_EIP) - 1, &b, 1);
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
  uc_hook_add(uc, &h, UC_HOOK_CODE, (void *)code_hook, NULL, 1, 0);
  if (!g_thread)
    pthread_create(&g_thread, NULL, cpu_thread, NULL);
}

static void stats(void) {
  fprintf(stderr, "[uc] E/S %lu, MMIO %lu, IRQ entregadas %lu, INT n %lu\n", g_stat_io, g_stat_mmio,
          g_stat_irq, g_stat_int);
}

int avm_test_open(const char *path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  int mode = va_arg(ap, int);
  va_end(ap);
  if (!strcmp(path, "/dev/aurora-vm")) {
    g_fake_fd = open("/dev/null", O_RDWR | O_CLOEXEC, 0);
    if (!uc)
      atexit(stats);
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
