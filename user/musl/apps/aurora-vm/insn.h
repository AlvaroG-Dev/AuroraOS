// insn.h — emulador de instrucciones x86 para los accesos MMIO de la mini-VM.
//
// Cuando el huesped toca una direccion que no es RAM (LAPIC, IOAPIC, memoria VGA
// planar...), el kernel sale con AVM_EXIT_MMIO y solo da la direccion fisica. Aqui
// se decodifica la instruccion en RIP y se ejecuta contra los dispositivos:
//   MOV (88-8B, C6/C7, A0-A3), MOVZX/MOVSX (0F B6/B7/BE/BF), MOVSXD (63),
//   ALU con memoria (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP 00-3B, 80/81/83), TEST (84/85,
//   F6/F7 /0), NOT/NEG (F6/F7 /2 /3), INC/DEC (FE/FF /0 /1), XCHG (86/87),
//   BT/BTS/BTR/BTC (0F A3/AB/B3/BB, 0F BA), CMPXCHG (0F B0/B1), XADD (0F C0/C1),
//   cadenas MOVS/STOS/LODS/CMPS/SCAS con REP (A4-AF), y MOVNTI (0F C3).
// En modo real, protegido de 16/32 bits y largo, con o sin paginacion (32 bits,
// PAE, 4 niveles).
//
// Depende de (definidas antes de incluir): gmem_read/gmem_write (memoria fisica
// del huesped: RAM o dispositivo) y gpt_read (tabla de paginas: solo RAM).
#ifndef AVM_INSN_H
#define AVM_INSN_H

static int gmem_read(uint64_t gpa, void *dst, unsigned n);
static int gmem_write(uint64_t gpa, const void *src, unsigned n);
static int gpt_read(uint64_t gpa, void *dst, unsigned n);
static void gpt_setbits(uint64_t gpa, uint64_t bits, unsigned n); // OR en una entrada (RAM)

static const char *g_insn_err; // motivo del ultimo fallo (diagnostico)

// ---------------------------------------------------------------------------
// Traduccion de direcciones
// ---------------------------------------------------------------------------
#define RFL_CF (1ULL << 0)
#define RFL_PF (1ULL << 2)
#define RFL_AF (1ULL << 4)
#define RFL_ZF (1ULL << 6)
#define RFL_SF (1ULL << 7)
#define RFL_DF (1ULL << 10)
#define RFL_OF (1ULL << 11)
#define RFL_ARITH (RFL_CF | RFL_PF | RFL_AF | RFL_ZF | RFL_SF | RFL_OF)

// Lineal -> fisica con las tablas del huesped. 0 o -1 (no presente).
// write: ademas marca Accedida en cada nivel y Sucia en la entrada final, como
// haria la CPU. Sin la marca de sucia, Windows puede tirar una pagina que el VMM
// escribio (por una instruccion emulada o por INS) creyendola sin cambios.
static int gva_walk(const struct avm_regs *r, uint64_t gva, uint64_t *gpa, int write);
static int gva_to_gpa(const struct avm_regs *r, uint64_t gva, uint64_t *gpa) {
  return gva_walk(r, gva, gpa, 0);
}
static int gva_walk(const struct avm_regs *r, uint64_t gva, uint64_t *gpa, int write) {
  if (!(r->cr0 & (1ULL << 31))) { // sin paginacion
    *gpa = (r->efer & (1ULL << 10)) ? gva : (gva & 0xFFFFFFFFULL);
    return 0;
  }
  const uint64_t AMASK = 0x000FFFFFFFFFF000ULL;
  if (r->efer & (1ULL << 10)) { // 4 niveles (LA57 no se anuncia)
    uint64_t t = r->cr3 & AMASK, e;
    for (int lvl = 3; lvl >= 0; lvl--) {
      uint64_t idx = (gva >> (12 + 9 * lvl)) & 0x1FF;
      if (gpt_read(t + idx * 8, &e, 8) < 0 || !(e & 1))
        return -1;
      int leaf = lvl == 0 || ((lvl == 1 || lvl == 2) && (e & 0x80));
      if (write)
        gpt_setbits(t + idx * 8, leaf ? 0x60 : 0x20, 8);
      if ((lvl == 1 || lvl == 2) && (e & 0x80)) { // 2 MiB / 1 GiB
        uint64_t sz = 1ULL << (12 + 9 * lvl);
        *gpa = ((e & AMASK) & ~(sz - 1)) | (gva & (sz - 1));
        return 0;
      }
      if (lvl == 0) {
        *gpa = (e & AMASK) | (gva & 0xFFF);
        return 0;
      }
      t = e & AMASK;
    }
    return -1;
  }
  gva &= 0xFFFFFFFFULL;
  if (r->cr4 & (1ULL << 5)) { // PAE de 32 bits
    uint64_t e;
    if (gpt_read((r->cr3 & 0xFFFFFFE0ULL) + ((gva >> 30) & 3) * 8, &e, 8) < 0 || !(e & 1))
      return -1;
    uint64_t t = e & AMASK;
    if (gpt_read(t + ((gva >> 21) & 0x1FF) * 8, &e, 8) < 0 || !(e & 1))
      return -1;
    if (write)
      gpt_setbits(t + ((gva >> 21) & 0x1FF) * 8, (e & 0x80) ? 0x60 : 0x20, 8);
    if (e & 0x80) {
      *gpa = ((e & AMASK) & ~0x1FFFFFULL) | (gva & 0x1FFFFF);
      return 0;
    }
    t = e & AMASK;
    if (gpt_read(t + ((gva >> 12) & 0x1FF) * 8, &e, 8) < 0 || !(e & 1))
      return -1;
    if (write)
      gpt_setbits(t + ((gva >> 12) & 0x1FF) * 8, 0x60, 8);
    *gpa = (e & AMASK) | (gva & 0xFFF);
    return 0;
  }
  uint32_t d;
  if (gpt_read((r->cr3 & 0xFFFFF000ULL) + ((gva >> 22) & 0x3FF) * 4, &d, 4) < 0 || !(d & 1))
    return -1;
  int big = (d & 0x80) && (r->cr4 & 0x10);
  if (write)
    gpt_setbits((r->cr3 & 0xFFFFF000ULL) + ((gva >> 22) & 0x3FF) * 4, big ? 0x60 : 0x20, 4);
  if (big) { // 4 MiB (PSE; PSE-36 en los bits 13-20)
    *gpa = ((uint64_t)(d & 0xFFC00000u)) | ((uint64_t)((d >> 13) & 0xFF) << 32) | (gva & 0x3FFFFF);
    return 0;
  }
  uint32_t t;
  if (gpt_read((d & 0xFFFFF000u) + ((gva >> 12) & 0x3FF) * 4, &t, 4) < 0 || !(t & 1))
    return -1;
  if (write)
    gpt_setbits((d & 0xFFFFF000u) + ((gva >> 12) & 0x3FF) * 4, 0x60, 4);
  *gpa = (t & 0xFFFFF000u) | (gva & 0xFFF);
  return 0;
}

// Lectura/escritura lineal (puede cruzar paginas; cada byte por su pagina).
static int lin_read(const struct avm_regs *r, uint64_t lin, void *dst, unsigned n) {
  uint8_t *d = (uint8_t *)dst;
  while (n) {
    uint64_t gpa;
    if (gva_to_gpa(r, lin, &gpa) < 0)
      return -1;
    unsigned chunk = 4096 - (unsigned)(lin & 0xFFF);
    if (chunk > n)
      chunk = n;
    if (gmem_read(gpa, d, chunk) < 0)
      return -1;
    d += chunk;
    lin += chunk;
    n -= chunk;
  }
  return 0;
}
static int lin_write(const struct avm_regs *r, uint64_t lin, const void *src, unsigned n) {
  const uint8_t *s = (const uint8_t *)src;
  while (n) {
    uint64_t gpa;
    if (gva_walk(r, lin, &gpa, 1) < 0)
      return -1;
    unsigned chunk = 4096 - (unsigned)(lin & 0xFFF);
    if (chunk > n)
      chunk = n;
    if (gmem_write(gpa, s, chunk) < 0)
      return -1;
    s += chunk;
    lin += chunk;
    n -= chunk;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Registros
// ---------------------------------------------------------------------------
static uint64_t *ireg_ptr(struct avm_regs *r, int n) {
  switch (n & 15) {
  case 0: return &r->rax; case 1: return &r->rcx; case 2: return &r->rdx;
  case 3: return &r->rbx; case 4: return &r->rsp; case 5: return &r->rbp;
  case 6: return &r->rsi; case 7: return &r->rdi; case 8: return &r->r8;
  case 9: return &r->r9; case 10: return &r->r10; case 11: return &r->r11;
  case 12: return &r->r12; case 13: return &r->r13; case 14: return &r->r14;
  default: return &r->r15;
  }
}
static uint64_t szmask(int size) {
  return size == 1 ? 0xFFULL : size == 2 ? 0xFFFFULL : size == 4 ? 0xFFFFFFFFULL : ~0ULL;
}
// rex != 0: AH..BH no existen (SPL..DIL en su lugar)
static uint64_t ireg_get(struct avm_regs *r, int n, int size, int rex) {
  if (size == 1 && !rex && n >= 4 && n < 8)
    return (*ireg_ptr(r, n - 4) >> 8) & 0xFF;
  return *ireg_ptr(r, n) & szmask(size);
}
static void ireg_set(struct avm_regs *r, int n, int size, int rex, uint64_t v) {
  if (size == 1 && !rex && n >= 4 && n < 8) {
    uint64_t *p = ireg_ptr(r, n - 4);
    *p = (*p & ~0xFF00ULL) | ((v & 0xFF) << 8);
    return;
  }
  uint64_t *p = ireg_ptr(r, n);
  switch (size) {
  case 1: *p = (*p & ~0xFFULL) | (v & 0xFF); break;
  case 2: *p = (*p & ~0xFFFFULL) | (v & 0xFFFF); break;
  case 4: *p = v & 0xFFFFFFFFULL; break; // 32 bits ponen a cero la mitad alta
  default: *p = v;
  }
}

// ---------------------------------------------------------------------------
// ALU con banderas (como el hardware; AF/PF/OF exactos)
// ---------------------------------------------------------------------------
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };

static uint64_t flags_szp(uint64_t res, int size) {
  uint64_t f = 0, m = szmask(size);
  res &= m;
  if (!res) f |= RFL_ZF;
  if (res >> (size * 8 - 1) & 1) f |= RFL_SF;
  if (!__builtin_parityll(res & 0xFF)) f |= RFL_PF;
  return f;
}

// Devuelve el resultado; *fl recibe las 6 banderas aritmeticas.
static uint64_t alu_op(int op, uint64_t a, uint64_t b, int size, uint64_t rfl, uint64_t *fl) {
  uint64_t m = szmask(size), sb = 1ULL << (size * 8 - 1);
  a &= m;
  b &= m;
  uint64_t res = 0, f = 0;
  uint64_t cin = (op == ALU_ADC || op == ALU_SBB) ? (rfl & RFL_CF ? 1 : 0) : 0;
  switch (op) {
  case ALU_ADD: case ALU_ADC: {
    res = (a + b + cin) & m;
    // acarreo: con 64 bits hay que mirar el desbordamiento sin perderlo
    int carry = size == 8 ? (res < a || (cin && res == a)) : ((a + b + cin) >> (size * 8)) & 1;
    if (carry) f |= RFL_CF;
    if ((~(a ^ b) & (a ^ res)) & sb) f |= RFL_OF;
    if ((a ^ b ^ res) & 0x10) f |= RFL_AF;
    break;
  }
  case ALU_SUB: case ALU_SBB: case ALU_CMP: {
    res = (a - b - cin) & m;
    int borrow = (a < b) || (cin && a == b);
    if (borrow) f |= RFL_CF;
    if (((a ^ b) & (a ^ res)) & sb) f |= RFL_OF;
    if ((a ^ b ^ res) & 0x10) f |= RFL_AF;
    break;
  }
  case ALU_OR: res = a | b; break;
  case ALU_AND: res = a & b; break;
  case ALU_XOR: res = a ^ b; break;
  }
  f |= flags_szp(res, size);
  *fl = f;
  return res;
}

static void set_arith_flags(struct avm_regs *r, uint64_t f, uint64_t which) {
  r->rflags = (r->rflags & ~which) | (f & which);
}

// ---------------------------------------------------------------------------
// Decodificacion
// ---------------------------------------------------------------------------
struct insn {
  uint8_t b[16];
  int n;            // bytes leidos
  int i;            // posicion actual
  int lm;           // modo de 64 bits
  int opsz, adsz;   // 2/4/8
  int rex, rex_w, rex_r, rex_x, rex_b;
  int rep;          // 0, 0xF2, 0xF3
  int seg;          // -1 o 0..5 (ES CS SS DS FS GS)
  int lock;
  int modrm, mod, reg, rm;
  int has_modrm;
  int64_t disp;
  int mem_base, mem_index, mem_scale; // -1 si no hay (para LEA de cadenas no hace falta)
  int rip_rel;
  int default_ss;   // base BP/SP: segmento SS por defecto
};

static int ins_byte(struct insn *d, uint8_t *out) {
  if (d->i >= d->n)
    return -1;
  *out = d->b[d->i++];
  return 0;
}

static int ins_imm(struct insn *d, int size, int64_t *out) {
  uint64_t v = 0;
  if (d->i + size > d->n)
    return -1;
  memcpy(&v, d->b + d->i, (size_t)size);
  d->i += size;
  switch (size) {
  case 1: *out = (int8_t)v; break;
  case 2: *out = (int16_t)v; break;
  case 4: *out = (int32_t)v; break;
  default: *out = (int64_t)v;
  }
  return 0;
}

// ModRM + SIB + desplazamiento (solo para conocer la longitud y el registro).
static int ins_modrm(struct insn *d) {
  uint8_t m;
  if (ins_byte(d, &m) < 0)
    return -1;
  d->has_modrm = 1;
  d->modrm = m;
  d->mod = m >> 6;
  d->reg = ((m >> 3) & 7) | (d->rex_r ? 8 : 0);
  d->rm = (m & 7) | (d->rex_b ? 8 : 0);
  if (d->mod == 3)
    return 0;
  int64_t disp = 0;
  if (d->adsz == 2) {
    int rm = m & 7;
    if (d->mod == 0 && rm == 6) {
      if (ins_imm(d, 2, &disp) < 0) return -1;
    } else if (d->mod == 1) {
      if (ins_imm(d, 1, &disp) < 0) return -1;
    } else if (d->mod == 2) {
      if (ins_imm(d, 2, &disp) < 0) return -1;
    }
    d->default_ss = (rm == 2 || rm == 3 || (rm == 6 && d->mod != 0));
  } else {
    int rm = m & 7;
    if (rm == 4) {
      uint8_t sib;
      if (ins_byte(d, &sib) < 0) return -1;
      int base = sib & 7;
      if (base == 5 && d->mod == 0) {
        if (ins_imm(d, 4, &disp) < 0) return -1;
      } else if (base == 4 || base == 5) {
        d->default_ss = 1;
      }
    } else if (rm == 5 && d->mod == 0) {
      if (ins_imm(d, 4, &disp) < 0) return -1;
      d->rip_rel = d->lm;
    } else if (rm == 5) {
      d->default_ss = 1;
    }
    if (d->mod == 1) {
      if (ins_imm(d, 1, &disp) < 0) return -1;
    } else if (d->mod == 2) {
      if (ins_imm(d, 4, &disp) < 0) return -1;
    }
  }
  d->disp = disp;
  return 0;
}

static const struct avm_seg *seg_by_index(const struct avm_regs *r, int s) {
  switch (s) {
  case 0: return &r->es;
  case 1: return &r->cs;
  case 2: return &r->ss;
  case 3: return &r->ds;
  case 4: return &r->fs;
  default: return &r->gs;
  }
}

static uint64_t seg_base(const struct avm_regs *r, const struct insn *d, int s) {
  if (d->lm && s != 4 && s != 5)
    return 0;
  return seg_by_index(r, s)->base;
}

// ---------------------------------------------------------------------------
// Cadenas: MOVS/STOS/LODS/CMPS/SCAS (con REP/REPE/REPNE)
// ---------------------------------------------------------------------------
static int insn_string(struct avm_regs *r, struct insn *d, uint8_t op) {
  int size = (op & 1) ? d->opsz : 1;
  uint64_t am = szmask(d->adsz);
  int64_t step = (r->rflags & RFL_DF) ? -(int64_t)size : (int64_t)size;
  int src_seg = d->seg >= 0 ? d->seg : 3;
  uint64_t cnt = d->rep ? (r->rcx & am) : 1;
  uint64_t si = r->rsi & am, di = r->rdi & am;
  int kind = (op >= 0xA4 && op <= 0xA5) ? 0   // MOVS
             : (op >= 0xAA && op <= 0xAB) ? 1 // STOS
             : (op >= 0xAC && op <= 0xAD) ? 2 // LODS
             : (op >= 0xA6 && op <= 0xA7) ? 3 // CMPS
                                          : 4; // SCAS
  uint64_t done = 0;
  int stop = 0;
  while (done < cnt && !stop) {
    uint64_t v = 0, w = 0;
    uint64_t lsrc = seg_base(r, d, src_seg) + si, ldst = seg_base(r, d, 0) + di;
    if (!d->lm) {
      lsrc &= 0xFFFFFFFFULL;
      ldst &= 0xFFFFFFFFULL;
    }
    switch (kind) {
    case 0:
      if (lin_read(r, lsrc, &v, (unsigned)size) < 0 || lin_write(r, ldst, &v, (unsigned)size) < 0)
        goto fault;
      si = (si + (uint64_t)step) & am;
      di = (di + (uint64_t)step) & am;
      break;
    case 1:
      v = r->rax & szmask(size);
      if (lin_write(r, ldst, &v, (unsigned)size) < 0)
        goto fault;
      di = (di + (uint64_t)step) & am;
      break;
    case 2:
      if (lin_read(r, lsrc, &v, (unsigned)size) < 0)
        goto fault;
      ireg_set(r, 0, size, 1, v);
      si = (si + (uint64_t)step) & am;
      break;
    case 3: case 4: {
      uint64_t fl;
      if (kind == 3) {
        if (lin_read(r, lsrc, &v, (unsigned)size) < 0 || lin_read(r, ldst, &w, (unsigned)size) < 0)
          goto fault;
        si = (si + (uint64_t)step) & am;
      } else {
        v = r->rax & szmask(size);
        if (lin_read(r, ldst, &w, (unsigned)size) < 0)
          goto fault;
      }
      di = (di + (uint64_t)step) & am;
      alu_op(ALU_CMP, v, w, size, r->rflags, &fl);
      set_arith_flags(r, fl, RFL_ARITH);
      if (d->rep == 0xF3 && !(fl & RFL_ZF)) stop = 1; // REPE: sale si distinto
      if (d->rep == 0xF2 && (fl & RFL_ZF)) stop = 1;  // REPNE: sale si igual
      break;
    }
    }
    done++;
  }
  // Actualizar punteros y contador con el tamano de direccion.
  {
    uint64_t *ps = &r->rsi, *pd = &r->rdi, *pc = &r->rcx;
    if (d->adsz == 2) {
      *ps = (*ps & ~0xFFFFULL) | si;
      *pd = (*pd & ~0xFFFFULL) | di;
      if (d->rep) *pc = (*pc & ~0xFFFFULL) | ((cnt - done) & 0xFFFF);
    } else {
      *ps = si;
      *pd = di;
      if (d->rep) *pc = (cnt - done) & am;
    }
  }
  return 0;
fault:
  g_insn_err = "fallo de pagina en una instruccion de cadena";
  return -1;
}

// ---------------------------------------------------------------------------
// Emulacion de una instruccion con operando en memoria de dispositivo.
// gpa: direccion fisica que provoco la salida. Actualiza *r (incluido RIP).
// Devuelve 0 o -1 (g_insn_err dice por que).
// ---------------------------------------------------------------------------
static int insn_emulate(struct avm_regs *r, uint64_t gpa) {
  struct insn d;
  memset(&d, 0, sizeof(d));
  d.seg = -1;
  d.lm = (r->efer & (1ULL << 10)) && (r->cs.ar & (1u << 13));
  int pe = (r->cr0 & 1) != 0;
  int d32 = pe && (r->cs.ar & (1u << 14)) != 0;
  d.opsz = d.lm || d32 ? 4 : 2;
  d.adsz = d.lm ? 8 : d32 ? 4 : 2;
  g_insn_err = NULL;

  uint64_t rip_lin = (d.lm ? 0 : r->cs.base) + r->rip;
  if (!d.lm)
    rip_lin &= 0xFFFFFFFFULL;
  // Leer hasta 15 bytes (los que se puedan si la instruccion acaba antes de una
  // pagina no presente).
  d.n = 0;
  for (int k = 0; k < 15; k++) {
    uint64_t pa;
    if (gva_to_gpa(r, rip_lin + (uint64_t)k, &pa) < 0 || gmem_read(pa, &d.b[k], 1) < 0)
      break;
    d.n++;
  }
  if (!d.n) {
    g_insn_err = "no se puede leer la instruccion";
    return -1;
  }

  uint8_t op;
  for (;;) {
    if (ins_byte(&d, &op) < 0)
      goto bad;
    if (op == 0x66) { d.opsz = (d.lm || d32) ? 2 : 4; continue; }
    if (op == 0x67) { d.adsz = d.lm ? 4 : (d32 ? 2 : 4); continue; }
    if (op == 0xF2 || op == 0xF3) { d.rep = op; continue; }
    if (op == 0xF0) { d.lock = 1; continue; }
    if (op == 0x26) { d.seg = 0; continue; }
    if (op == 0x2E) { d.seg = 1; continue; }
    if (op == 0x36) { d.seg = 2; continue; }
    if (op == 0x3E) { d.seg = 3; continue; }
    if (op == 0x64) { d.seg = 4; continue; }
    if (op == 0x65) { d.seg = 5; continue; }
    break;
  }
  if (d.lm && (op & 0xF0) == 0x40) {
    d.rex = op;
    d.rex_w = (op >> 3) & 1;
    d.rex_r = (op >> 2) & 1;
    d.rex_x = (op >> 1) & 1;
    d.rex_b = op & 1;
    if (d.rex_w)
      d.opsz = 8;
    if (ins_byte(&d, &op) < 0)
      goto bad;
  }

  int two = 0;
  if (op == 0x0F) {
    two = 1;
    if (ins_byte(&d, &op) < 0)
      goto bad;
  }

  uint64_t fl;
  if (!two) {
    // Cadenas
    if (op >= 0xA4 && op <= 0xAF && op != 0xA8 && op != 0xA9) {
      if (insn_string(r, &d, op) < 0)
        return -1;
      goto done;
    }
    // MOV AL/AX/EAX/RAX <-> [moffs]
    if (op >= 0xA0 && op <= 0xA3) {
      int size = (op & 1) ? d.opsz : 1;
      d.i += d.adsz; // desplazamiento absoluto del tamano de direccion
      if (d.i > d.n) goto bad;
      uint64_t v = 0;
      if (op >= 0xA2) {
        v = ireg_get(r, 0, size, 1);
        if (gmem_write(gpa, &v, (unsigned)size) < 0) goto bad_mem;
      } else {
        if (gmem_read(gpa, &v, (unsigned)size) < 0) goto bad_mem;
        ireg_set(r, 0, size, 1, v);
      }
      goto done;
    }
    // ALU r/m, reg / reg, r/m: 00-3B (patrones x0-x3 de cada grupo de 8)
    if (op < 0x40 && (op & 7) < 4) {
      int aop = op >> 3;
      int size = (op & 1) ? d.opsz : 1;
      int to_reg = (op & 2) != 0;
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      uint64_t m = 0;
      if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
      uint64_t rv = ireg_get(r, d.reg, size, d.rex);
      uint64_t a = to_reg ? rv : m, b = to_reg ? m : rv;
      uint64_t res = alu_op(aop, a, b, size, r->rflags, &fl);
      if (aop == ALU_OR || aop == ALU_AND || aop == ALU_XOR)
        fl &= ~(RFL_CF | RFL_OF | RFL_AF);
      set_arith_flags(r, fl, RFL_ARITH);
      if (aop != ALU_CMP) {
        if (to_reg)
          ireg_set(r, d.reg, size, d.rex, res);
        else if (gmem_write(gpa, &res, (unsigned)size) < 0)
          goto bad_mem;
      }
      goto done;
    }
    switch (op) {
    case 0x63: { // MOVSXD reg, r/m32 (modo largo)
      if (!d.lm || ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      int32_t v = 0;
      if (gmem_read(gpa, &v, 4) < 0) goto bad_mem;
      ireg_set(r, d.reg, d.opsz, d.rex, (uint64_t)(int64_t)v);
      goto done;
    }
    case 0x80: case 0x81: case 0x83: { // grupo 1 con inmediato
      int size = op == 0x80 ? 1 : d.opsz;
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      int64_t imm;
      int isz = op == 0x81 ? (size == 2 ? 2 : 4) : 1;
      if (ins_imm(&d, isz, &imm) < 0) goto bad;
      uint64_t m = 0;
      if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
      int aop = d.reg & 7;
      uint64_t res = alu_op(aop, m, (uint64_t)imm, size, r->rflags, &fl);
      if (aop == ALU_OR || aop == ALU_AND || aop == ALU_XOR)
        fl &= ~(RFL_CF | RFL_OF | RFL_AF);
      set_arith_flags(r, fl, RFL_ARITH);
      if (aop != ALU_CMP && gmem_write(gpa, &res, (unsigned)size) < 0)
        goto bad_mem;
      goto done;
    }
    case 0x84: case 0x85: { // TEST r/m, reg
      int size = op == 0x84 ? 1 : d.opsz;
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      uint64_t m = 0;
      if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
      alu_op(ALU_AND, m, ireg_get(r, d.reg, size, d.rex), size, r->rflags, &fl);
      fl &= ~(RFL_CF | RFL_OF | RFL_AF);
      set_arith_flags(r, fl, RFL_ARITH);
      goto done;
    }
    case 0x86: case 0x87: { // XCHG r/m, reg
      int size = op == 0x86 ? 1 : d.opsz;
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      uint64_t m = 0;
      if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
      uint64_t rv = ireg_get(r, d.reg, size, d.rex);
      if (gmem_write(gpa, &rv, (unsigned)size) < 0) goto bad_mem;
      ireg_set(r, d.reg, size, d.rex, m);
      goto done;
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
      int size = (op & 1) ? d.opsz : 1;
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      uint64_t v = 0;
      if (op <= 0x89) {
        v = ireg_get(r, d.reg, size, d.rex);
        if (gmem_write(gpa, &v, (unsigned)size) < 0) goto bad_mem;
      } else {
        if (gmem_read(gpa, &v, (unsigned)size) < 0) goto bad_mem;
        ireg_set(r, d.reg, size, d.rex, v);
      }
      goto done;
    }
    case 0x8C: { // MOV r/m16, Sreg
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      uint16_t v = seg_by_index(r, d.reg & 7)->selector;
      if (gmem_write(gpa, &v, 2) < 0) goto bad_mem;
      goto done;
    }
    case 0xC6: case 0xC7: {
      int size = op == 0xC6 ? 1 : d.opsz;
      if (ins_modrm(&d) < 0 || d.mod == 3 || (d.reg & 7) != 0) goto bad;
      int64_t imm;
      if (ins_imm(&d, size == 1 ? 1 : size == 2 ? 2 : 4, &imm) < 0) goto bad;
      uint64_t v = (uint64_t)imm & szmask(size);
      if (gmem_write(gpa, &v, (unsigned)size) < 0) goto bad_mem;
      goto done;
    }
    case 0xF6: case 0xF7: { // grupo 3
      int size = op == 0xF6 ? 1 : d.opsz;
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      int sub = d.reg & 7;
      uint64_t m = 0;
      if (sub == 0 || sub == 1) { // TEST r/m, imm
        int64_t imm;
        if (ins_imm(&d, size == 1 ? 1 : size == 2 ? 2 : 4, &imm) < 0) goto bad;
        if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
        alu_op(ALU_AND, m, (uint64_t)imm, size, r->rflags, &fl);
        fl &= ~(RFL_CF | RFL_OF | RFL_AF);
        set_arith_flags(r, fl, RFL_ARITH);
        goto done;
      }
      if (sub == 2 || sub == 3) {
        if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
        uint64_t res;
        if (sub == 2) {
          res = ~m & szmask(size); // NOT: sin banderas
        } else {
          res = alu_op(ALU_SUB, 0, m, size, r->rflags, &fl); // NEG
          set_arith_flags(r, fl, RFL_ARITH);
        }
        if (gmem_write(gpa, &res, (unsigned)size) < 0) goto bad_mem;
        goto done;
      }
      g_insn_err = "MUL/DIV con operando en un dispositivo";
      goto bad_msg;
    }
    case 0xFE: case 0xFF: { // INC/DEC r/m
      int size = op == 0xFE ? 1 : d.opsz;
      if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
      int sub = d.reg & 7;
      if (sub > 1) {
        g_insn_err = "CALL/JMP/PUSH con operando en un dispositivo";
        goto bad_msg;
      }
      uint64_t m = 0;
      if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
      uint64_t res = alu_op(sub == 0 ? ALU_ADD : ALU_SUB, m, 1, size, r->rflags, &fl);
      set_arith_flags(r, fl, RFL_ARITH & ~RFL_CF); // INC/DEC no tocan CF
      if (gmem_write(gpa, &res, (unsigned)size) < 0) goto bad_mem;
      goto done;
    }
    default:
      goto bad;
    }
  }

  // Dos bytes (0F xx)
  switch (op) {
  case 0xB6: case 0xB7: case 0xBE: case 0xBF: { // MOVZX / MOVSX
    if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
    int src = (op & 1) ? 2 : 1;
    uint64_t v = 0;
    if (gmem_read(gpa, &v, (unsigned)src) < 0) goto bad_mem;
    if (op >= 0xBE)
      v = src == 1 ? (uint64_t)(int64_t)(int8_t)v : (uint64_t)(int64_t)(int16_t)v;
    ireg_set(r, d.reg, d.opsz, d.rex, v & szmask(d.opsz));
    goto done;
  }
  case 0xC3: { // MOVNTI m, reg
    if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
    uint64_t v = ireg_get(r, d.reg, d.opsz, d.rex);
    if (gmem_write(gpa, &v, (unsigned)d.opsz) < 0) goto bad_mem;
    goto done;
  }
  case 0xB0: case 0xB1: { // CMPXCHG r/m, reg
    int size = op == 0xB0 ? 1 : d.opsz;
    if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
    uint64_t m = 0;
    if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
    uint64_t acc = ireg_get(r, 0, size, 1);
    alu_op(ALU_CMP, acc, m, size, r->rflags, &fl);
    set_arith_flags(r, fl, RFL_ARITH);
    if (fl & RFL_ZF) {
      uint64_t v = ireg_get(r, d.reg, size, d.rex);
      if (gmem_write(gpa, &v, (unsigned)size) < 0) goto bad_mem;
    } else {
      // el hardware escribe el valor leido de vuelta (ciclo de bloqueo)
      if (gmem_write(gpa, &m, (unsigned)size) < 0) goto bad_mem;
      ireg_set(r, 0, size, 1, m);
    }
    goto done;
  }
  case 0xC0: case 0xC1: { // XADD r/m, reg
    int size = op == 0xC0 ? 1 : d.opsz;
    if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
    uint64_t m = 0;
    if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
    uint64_t rv = ireg_get(r, d.reg, size, d.rex);
    uint64_t res = alu_op(ALU_ADD, m, rv, size, r->rflags, &fl);
    set_arith_flags(r, fl, RFL_ARITH);
    if (gmem_write(gpa, &res, (unsigned)size) < 0) goto bad_mem;
    ireg_set(r, d.reg, size, d.rex, m);
    goto done;
  }
  case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xBA: { // BT/BTS/BTR/BTC
    if (ins_modrm(&d) < 0 || d.mod == 3) goto bad;
    int size = d.opsz;
    int kind;
    int64_t bit;
    if (op == 0xBA) {
      int64_t imm;
      if ((d.reg & 7) < 4) goto bad;
      if (ins_imm(&d, 1, &imm) < 0) goto bad;
      kind = (d.reg & 7) - 4; // 0 BT, 1 BTS, 2 BTR, 3 BTC
      bit = imm & (size * 8 - 1);
    } else {
      kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
      bit = (int64_t)ireg_get(r, d.reg, size, d.rex);
      // Con registro el indice puede salirse del operando: solo se admite dentro.
      if (bit < 0 || bit >= size * 8) {
        g_insn_err = "BT con indice fuera del operando sobre un dispositivo";
        goto bad_msg;
      }
    }
    uint64_t m = 0;
    if (gmem_read(gpa, &m, (unsigned)size) < 0) goto bad_mem;
    int old = (int)((m >> bit) & 1);
    r->rflags = (r->rflags & ~RFL_CF) | (old ? RFL_CF : 0);
    if (kind) {
      if (kind == 1) m |= 1ULL << bit;
      else if (kind == 2) m &= ~(1ULL << bit);
      else m ^= 1ULL << bit;
      if (gmem_write(gpa, &m, (unsigned)size) < 0) goto bad_mem;
    }
    goto done;
  }
  default:
    goto bad;
  }

done:
  if (d.i > d.n)
    goto bad;
  if (d.lm)
    r->rip += (uint64_t)d.i;
  else if (d32)
    r->rip = (r->rip + (uint64_t)d.i) & 0xFFFFFFFFULL;
  else
    r->rip = (r->rip + (uint64_t)d.i) & 0xFFFF; // codigo de 16 bits: IP de 16 bits
  return 0;
bad:
  if (!g_insn_err)
    g_insn_err = "instruccion no soportada";
  return -1;
bad_mem:
  g_insn_err = "fallo de acceso a la memoria del huesped";
  return -1;
bad_msg:
  return -1;
}

// Bytes de la instruccion en RIP (para el mensaje de error).
static int insn_bytes(const struct avm_regs *r, uint8_t *out, int max) {
  int lm = (r->efer & (1ULL << 10)) && (r->cs.ar & (1u << 13));
  uint64_t lin = (lm ? 0 : r->cs.base) + r->rip;
  int n = 0;
  for (; n < max; n++) {
    uint64_t pa;
    if (gva_to_gpa(r, lin + (uint64_t)n, &pa) < 0 || gmem_read(pa, &out[n], 1) < 0)
      break;
  }
  return n;
}

#endif
