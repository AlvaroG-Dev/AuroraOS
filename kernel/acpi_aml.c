// kernel/acpi_aml.c
// Intérprete AML. Ver acpi_aml.h para alcance.
//
// Correcciones respecto a la versión anterior:
//   * PkgLength multi-byte: 4 bits en el primer byte, no 6.
//   * If/While: el Predicate va DENTRO del PkgLength, después de él.
//   * PackageOp: NumElements es 1 byte (no variable).
//   * TermList: ya no aborta ante un opcode desconocido; lo salta.
//   * +20 opcodes nuevos (IndexField, BankField, CreateField, etc.).
//   * _OSI implementado.
//   * OperationRegion (SystemMemory/SystemIO/PCI_Config) usable.

#include "acpi_aml.h"
#include "acpi.h"
#include "heap.h"
#include "io.h"
#include "klog.h"
#include "paging.h"
#include "string.h"

// ===========================================================================
// Opcodes AML — referencias: ACPI 6.4 §20.2
// ===========================================================================
// Simple data
#define AML_ZERO_OP 0x00
#define AML_ONE_OP 0x01
#define AML_ALIAS_OP 0x06
#define AML_NAME_OP 0x08
#define AML_BYTE_PREFIX 0x0A
#define AML_WORD_PREFIX 0x0B
#define AML_DWORD_PREFIX 0x0C
#define AML_STRING_PREFIX 0x0D
#define AML_QWORD_PREFIX 0x0E

// Namespace modifiers
#define AML_SCOPE_OP 0x10
#define AML_BUFFER_OP 0x11
#define AML_PACKAGE_OP 0x12
#define AML_VAR_PACKAGE_OP 0x13
#define AML_METHOD_OP 0x14
#define AML_EXTERNAL_OP 0x15

// Extended prefix (0x5B)
#define AML_EXT_OP_PREFIX 0x5B
#define AML_EXT_MUTEX_OP 0x01
#define AML_EXT_EVENT_OP 0x02
#define AML_EXT_COND_REF_OF 0x12
#define AML_EXT_CREATE_FIELD 0x13
#define AML_EXT_LOAD_TABLE 0x1F
#define AML_EXT_LOAD 0x20
#define AML_EXT_STALL 0x21
#define AML_EXT_SLEEP 0x22
#define AML_EXT_ACQUIRE 0x23
#define AML_EXT_SIGNAL 0x24
#define AML_EXT_WAIT 0x25
#define AML_EXT_RESET 0x26
#define AML_EXT_RELEASE 0x27
#define AML_EXT_FROM_BCD 0x28
#define AML_EXT_TO_BCD 0x29
#define AML_EXT_UNLOAD 0x2A
#define AML_EXT_REVISION 0x30
#define AML_EXT_DEBUG 0x31
#define AML_EXT_FATAL 0x32
#define AML_EXT_TIMER 0x33
#define AML_EXT_OPREGION 0x80
#define AML_EXT_FIELD 0x81
#define AML_EXT_DEVICE 0x82
#define AML_EXT_PROCESSOR 0x83
#define AML_EXT_POWERRES 0x84
#define AML_EXT_THERMALZONE 0x85
#define AML_EXT_INDEX_FIELD 0x86
#define AML_EXT_BANK_FIELD 0x87

// Term obj
#define AML_STORE_OP 0x70
#define AML_REF_OF_OP 0x71
#define AML_ADD_OP 0x72
#define AML_CONCAT_OP 0x73
#define AML_SUBTRACT_OP 0x74
#define AML_INCREMENT_OP 0x75
#define AML_DECREMENT_OP 0x76
#define AML_MULTIPLY_OP 0x77
#define AML_DIVIDE_OP 0x78
#define AML_SHIFT_LEFT_OP 0x79
#define AML_SHIFT_RIGHT_OP 0x7A
#define AML_AND_OP 0x7B
#define AML_NAND_OP 0x7C
#define AML_OR_OP 0x7D
#define AML_NOR_OP 0x7E
#define AML_XOR_OP 0x7F
#define AML_NOT_OP 0x80
#define AML_BITWISE_NOT_OP 0x80
#define AML_FIND_SET_LEFT 0x81
#define AML_FIND_SET_RIGHT 0x82
#define AML_DEREF_OF_OP 0x83
#define AML_CONCAT_RES_OP 0x84
#define AML_MOD_OP 0x85
#define AML_NOTIFY_OP 0x86
#define AML_SIZE_OF_OP 0x87
#define AML_INDEX_OP 0x88
#define AML_MATCH_OP 0x89
#define AML_CREATE_DWORD 0x8A
#define AML_CREATE_WORD 0x8B
#define AML_CREATE_BYTE 0x8C
#define AML_CREATE_BITFIELD 0x8D
#define AML_OBJECT_TYPE_OP 0x8E
#define AML_CREATE_QWORD 0x8F
#define AML_LAND_OP 0x90
#define AML_LOR_OP 0x91
#define AML_LNOT_OP 0x92
#define AML_LEQUAL_OP 0x93
#define AML_LGREATER_OP 0x94
#define AML_LLESS_OP 0x95
#define AML_TO_BUFFER_OP 0x96
#define AML_TO_DECSTRING_OP 0x97
#define AML_TO_HEXSTRING_OP 0x98
#define AML_TO_INTEGER_OP 0x99
#define AML_TO_STRING_OP 0x9C
#define AML_COPY_OBJECT_OP 0x9D
#define AML_MID_OP 0x9E
#define AML_CONTINUE_OP 0x9F
#define AML_IF_OP 0xA0
#define AML_ELSE_OP 0xA1
#define AML_WHILE_OP 0xA2
#define AML_NOOP_OP 0xA3
#define AML_RETURN_OP 0xA4
#define AML_BREAK_OP 0xA5
#define AML_BREAKPOINT_OP 0xCC
#define AML_ONES_OP 0xFF

// Métodos y Args/Locals: 0x60..0x6F son Arg0..6, Local0..7
#define AML_LOCAL0_OP 0x60
#define AML_LOCAL7_OP 0x67
#define AML_ARG0_OP 0x68
#define AML_ARG6_OP 0x6E

// Comparison operands (usados por Match)
#define AML_MTR_OP 0x00
#define AML_MEQ_OP 0x01
#define AML_MLE_OP 0x02
#define AML_MLT_OP 0x03
#define AML_MGE_OP 0x04
#define AML_MGT_OP 0x05

// ===========================================================================
// Estado global
// ===========================================================================
#define AML_MAX_NODES 8192
#define AML_EVAL_STACK 512
#define AML_MAX_LOCALS 8
#define AML_MAX_ARGS 7
#define AML_MAX_DEPTH 64         // profundidad de anidamiento de eval
#define AML_WHILE_MAX_ITER 10000 // guardarraíl anti bucles infinitos

static aml_node_t *g_root = NULL;
static aml_node_t *g_node_pool = NULL;
static int g_node_pool_used = 0;
static int g_ready = 0;

// ===========================================================================
// Memoria
// ===========================================================================
static aml_node_t *alloc_node(void) {
  if (!g_node_pool) {
    g_node_pool = (aml_node_t *)kmalloc(sizeof(aml_node_t) * AML_MAX_NODES);
    if (!g_node_pool)
      return NULL;
    memset(g_node_pool, 0, sizeof(aml_node_t) * AML_MAX_NODES);
  }
  if (g_node_pool_used >= AML_MAX_NODES)
    return NULL;
  aml_node_t *n = &g_node_pool[g_node_pool_used++];
  memset(n, 0, sizeof(*n));
  return n;
}

static aml_object_t *alloc_obj(aml_type_t type) {
  aml_object_t *o = (aml_object_t *)kmalloc(sizeof(aml_object_t));
  if (!o)
    return NULL;
  memset(o, 0, sizeof(*o));
  o->type = type;
  return o;
}

static aml_object_t *make_integer(uint64_t v) {
  aml_object_t *o = alloc_obj(AML_INTEGER);
  if (o)
    o->integer = v;
  return o;
}

static aml_object_t *make_string(const char *s, uint32_t len) {
  aml_object_t *o = alloc_obj(AML_STRING);
  if (!o)
    return NULL;
  o->string.data = (char *)kmalloc(len + 1);
  if (!o->string.data) {
    kfree(o);
    return NULL;
  }
  memcpy(o->string.data, s, len);
  o->string.data[len] = '\0';
  o->string.len = len;
  return o;
}

static aml_object_t *make_buffer(const uint8_t *data, uint32_t len) {
  aml_object_t *o = alloc_obj(AML_BUFFER);
  if (!o)
    return NULL;
  o->buffer.data = (uint8_t *)kmalloc(len ? len : 1);
  if (!o->buffer.data) {
    kfree(o);
    return NULL;
  }
  if (len && data)
    memcpy(o->buffer.data, data, len);
  o->buffer.len = len;
  return o;
}

static aml_object_t *make_package(uint32_t count) {
  aml_object_t *o = alloc_obj(AML_PACKAGE);
  if (!o)
    return NULL;
  o->package.count = count;
  if (count == 0) {
    o->package.elems = NULL;
    return o;
  }
  o->package.elems = (aml_object_t **)kmalloc(sizeof(aml_object_t *) * count);
  if (!o->package.elems) {
    kfree(o);
    return NULL;
  }
  memset(o->package.elems, 0, sizeof(aml_object_t *) * count);
  return o;
}

// ===========================================================================
// Namespace
// ===========================================================================
static aml_node_t *find_child(aml_node_t *parent, const char *name) {
  if (!parent)
    return NULL;
  for (aml_node_t *c = parent->children; c; c = c->sibling) {
    if (memcmp(c->name, name, 4) == 0)
      return c;
  }
  return NULL;
}

// Crea/encuentra un nodo por path absoluto tipo "\_SB_.PCI0.RP01".
// Los segmentos de menos de 4 chars se rellenan con '_'.
static aml_node_t *ensure_path(const char *path) {
  if (!g_root)
    return NULL;
  aml_node_t *cur = g_root;
  const char *p = path;
  while (*p == '\\')
    p++;
  while (*p) {
    char name[5] = {'_', '_', '_', '_', '\0'};
    int n = 0;
    while (*p && *p != '.' && n < 4)
      name[n++] = *p++;
    while (*p && *p != '.')
      p++;
    if (n == 0)
      break;
    if (*p == '.')
      p++;
    aml_node_t *child = find_child(cur, name);
    if (!child) {
      child = alloc_node();
      if (!child)
        return NULL;
      memcpy(child->name, name, 5);
      child->parent = cur;
      child->sibling = cur->children;
      cur->children = child;
    }
    cur = child;
  }
  return cur;
}

// Convierte un path normalizado a array de 4-char names. Cuenta segmentos.
static int path_segments(const char *path, char (*out)[5], int max) {
  int n = 0;
  const char *p = path;
  while (*p == '\\' || *p == '^')
    p++;
  while (*p && n < max) {
    char name[5] = {'_', '_', '_', '_', '\0'};
    int k = 0;
    while (*p && *p != '.' && k < 4)
      name[k++] = *p++;
    while (*p && *p != '.')
      p++;
    if (*p == '.')
      p++;
    memcpy(out[n], name, 5);
    n++;
  }
  return n;
}

// Busca un nodo por path absoluto sin crear nada.
static aml_node_t *lookup_path_abs(const char *path) {
  if (!g_root)
    return NULL;
  char segs[32][5];
  int n = path_segments(path, segs, 32);
  aml_node_t *cur = g_root;
  for (int i = 0; i < n; i++) {
    cur = find_child(cur, segs[i]);
    if (!cur)
      return NULL;
  }
  return cur;
}

// ===========================================================================
// PkgLength
// ===========================================================================
// Decodifica un PkgLength desde code[pos]. Actualiza pos tras el PkgLength.
// Devuelve el tamaño total del paquete (PkgLength inclusive).
static uint32_t decode_pkglength(const uint8_t *code, size_t len, size_t *pos) {
  if (*pos >= len)
    return 0;
  uint8_t b0 = code[*pos];
  int extra = (b0 >> 6) & 3;
  if (extra == 0) {
    (*pos)++;
    return (uint32_t)(b0 & 0x3F);
  }
  if (*pos + extra >= len) {
    (*pos)++;
    return 0;
  }
  uint32_t v = b0 & 0x0F;
  (*pos)++;
  for (int i = 0; i < extra; i++) {
    v |= ((uint32_t)code[*pos]) << (4 + i * 8);
    (*pos)++;
  }
  return v;
}

// ===========================================================================
// NameString / NameSeg
// ===========================================================================
// ===========================================================================
// NameString — Referencia a un objeto del namespace.
//
// Gramática AML (ACPI 6.4 §20.2.2):
//
//   NameString := <RootChar NamePath> | <PrefixPath NamePath>
//   NamePath   := NameSeg | DualNamePath | MultiNamePath | NullName
//   PrefixPath := Nothing | '^' PrefixPath
//   RootChar   := '\'
//   DualNamePath  := DualNamePrefix NameSeg NameSeg
//   MultiNamePath := MultiNamePrefix SegCount NameSeg(SegCount)
//   NullName      := 0x00
//
//   DualNamePrefix  := 0x2E
//   MultiNamePrefix := 0x2F
//
// La salida se normaliza a un string con prefijos '^'/'\\' seguidos de
// NameSegs separados por '.'. Ejemplos:
//   "\" + 0x2F 02 "_SB_" "PCI0"    → "\_SB_.PCI0"
//   "^" + 0x2E "PCI0" "HPET"       → "^PCI0.HPET"
//   "ABCD"                          → "ABCD"
//
// Errores:
//   -1  si el buffer de salida es demasiado pequeño
//   -1  si se sale de los límites de `code`
// ===========================================================================
static int read_namestring(const uint8_t *code, size_t len, size_t *pos,
                           char *out, size_t outsz) {
  if (!code || !pos || !out || outsz < 2)
    return -1;

  size_t o = 0;

  // ---------------------------------------------------------------------
  // 1. Prefijos: '\' (RootChar) y '^' (PrefixPath).
  //    Puede haber varios '^' seguidos, y opcionalmente un '\' al final.
  // ---------------------------------------------------------------------
  while (*pos < len && (code[*pos] == '\\' || code[*pos] == '^')) {
    if (o + 2 >= outsz)
      return -1;
    out[o++] = (char)code[(*pos)++];
  }

  if (*pos >= len) {
    // Prefijos sin NamePath. NameString vacío no es válido, pero no
    // dejamos el buffer sin NUL.
    out[o] = '\0';
    return -1;
  }

  // ---------------------------------------------------------------------
  // 2. NullName (0x00). Solo aparece como Target de algunos opcodes,
  //    pero por completitud lo soportamos.
  // ---------------------------------------------------------------------
  if (code[*pos] == 0x00) {
    (*pos)++;
    out[o] = '\0';
    return (int)o;
  }

  // ---------------------------------------------------------------------
  // 3. DualNamePrefix (0x2E): 2 NameSegs seguidos = 8 bytes.
  // ---------------------------------------------------------------------
  if (code[*pos] == 0x2E) {
    (*pos)++;
    for (int seg = 0; seg < 2; seg++) {
      if (*pos + 4 > len)
        return -1;
      if (o + 6 >= outsz) // 4 chars + '.' (o '\0') + margen
        return -1;
      for (int k = 0; k < 4; k++)
        out[o++] = (char)code[(*pos)++];
      if (seg == 0)
        out[o++] = '.';
    }
    out[o] = '\0';
    return (int)o;
  }

  // ---------------------------------------------------------------------
  // 4. MultiNamePrefix (0x2F): SegCount (1 byte) + N NameSegs (4N bytes).
  // ---------------------------------------------------------------------
  if (code[*pos] == 0x2F) {
    (*pos)++;
    if (*pos >= len)
      return -1;
    uint8_t n = code[(*pos)++];
    if (n == 0) {
      // MultiName con 0 segmentos: degenerado. Tratarlo como NullName.
      out[o] = '\0';
      return (int)o;
    }
    for (uint8_t seg = 0; seg < n; seg++) {
      if (*pos + 4 > len)
        return -1;
      if (o + 6 >= outsz)
        return -1;
      for (int k = 0; k < 4; k++)
        out[o++] = (char)code[(*pos)++];
      if (seg + 1 < n)
        out[o++] = '.';
    }
    out[o] = '\0';
    return (int)o;
  }

  // ---------------------------------------------------------------------
  // 5. NameSeg simple: exactamente 4 bytes.
  // ---------------------------------------------------------------------
  if (*pos + 4 > len)
    return -1;
  if (o + 5 >= outsz)
    return -1;
  for (int k = 0; k < 4; k++)
    out[o++] = (char)code[(*pos)++];
  out[o] = '\0';
  return (int)o;
}

// Resuelve un NameString relativo a `scope`, devolviendo el path
// absoluto (con '\' inicial). Maneja ^ y \.
static void resolve_name(aml_node_t *scope, const char *name, char *out,
                         size_t outsz) {
  size_t o = 0;
  const char *p = name;
  aml_node_t *base = scope;

  if (*p == '\\') {
    base = NULL;
    p++;
  } else {
    // Retroceder por cada ^
    while (*p == '^') {
      if (base && base->parent)
        base = base->parent;
      p++;
    }
  }

  out[o++] = '\\';

  // Path del scope base (de arriba hacia abajo)
  if (base && base != g_root) {
    // Recopilar segmentos en orden inverso
    char stack[32][5];
    int depth = 0;
    aml_node_t *n = base;
    while (n && n != g_root && depth < 32) {
      memcpy(stack[depth++], n->name, 4);
      stack[depth - 1][4] = '\0';
      n = n->parent;
    }
    // Escribir al revés
    for (int i = depth - 1; i >= 0; i--) {
      for (int k = 0; k < 4 && o + 1 < outsz; k++)
        out[o++] = stack[i][k];
      if (o + 1 < outsz)
        out[o++] = '.';
    }
  }

  // Añadir el nombre relativo
  while (*p && o + 1 < outsz)
    out[o++] = *p++;
  out[o] = '\0';
}

// ===========================================================================
// Parser: forward decls
// ===========================================================================
typedef struct {
  const uint8_t *data;
  size_t len;
  size_t pos;
  aml_node_t *scope;
} parse_ctx_t;

static aml_object_t *parse_term_arg(parse_ctx_t *c);
static void parse_term_list(parse_ctx_t *c, size_t end_pos);
static void parse_field_list(parse_ctx_t *c, size_t end_pos);

// ===========================================================================
// Parseo de elementos sueltos (data objects)
// ===========================================================================
// Forward declaration: method invocations inside data-object expressions must
// consume their encoded arguments so argument bytes are not reinterpreted as
// independent AML opcodes by the surrounding TermList parser.
static int aml_skip_term(parse_ctx_t *c, size_t limit, unsigned depth);

static aml_object_t *parse_data_object(parse_ctx_t *c) {
  if (c->pos >= c->len)
    return make_integer(0);
  uint8_t op = c->data[c->pos];

  switch (op) {
  case AML_ZERO_OP:
    c->pos++;
    return make_integer(0);
  case AML_ONE_OP:
    c->pos++;
    return make_integer(1);
  case AML_ONES_OP:
    c->pos++;
    return make_integer(0xFFFFFFFFFFFFFFFFULL);

  case AML_BYTE_PREFIX: {
    if (c->len - c->pos < 2) {
      c->pos = c->len;
      LOG_WARN("[AML] ByteConst truncado");
      return make_integer(0);
    }
    c->pos++;
    return make_integer(c->data[c->pos++]);
  }
  case AML_WORD_PREFIX: {
    if (c->len - c->pos < 3) {
      c->pos = c->len;
      LOG_WARN("[AML] WordConst truncado");
      return make_integer(0);
    }
    c->pos++;
    uint64_t v = c->data[c->pos] | ((uint64_t)c->data[c->pos + 1] << 8);
    c->pos += 2;
    return make_integer(v);
  }
  case AML_DWORD_PREFIX: {
    if (c->len - c->pos < 5) {
      c->pos = c->len;
      LOG_WARN("[AML] DWordConst truncado");
      return make_integer(0);
    }
    c->pos++;
    uint64_t v = c->data[c->pos] | ((uint64_t)c->data[c->pos + 1] << 8) |
                 ((uint64_t)c->data[c->pos + 2] << 16) |
                 ((uint64_t)c->data[c->pos + 3] << 24);
    c->pos += 4;
    return make_integer(v);
  }
  case AML_QWORD_PREFIX: {
    if (c->len - c->pos < 9) {
      c->pos = c->len;
      LOG_WARN("[AML] QWordConst truncado");
      return make_integer(0);
    }
    c->pos++;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
      v |= ((uint64_t)c->data[c->pos + i]) << (i * 8);
    c->pos += 8;
    return make_integer(v);
  }
  case AML_STRING_PREFIX: {
    c->pos++;
    size_t start = c->pos;
    while (c->pos < c->len && c->data[c->pos] != 0)
      c->pos++;
    size_t n = c->pos - start;
    if (c->pos < c->len)
      c->pos++;
    return make_string((const char *)(c->data + start), (uint32_t)n);
  }

  case AML_BUFFER_OP: {
    c->pos++;
    size_t pkg_start = c->pos;
    size_t outer_len = c->len;
    uint32_t pkglen = decode_pkglength(c->data, outer_len, &c->pos);
    if (!pkglen || pkg_start > outer_len || pkglen > outer_len - pkg_start) {
      LOG_WARN("[AML] Buffer con PkgLength inválido");
      c->pos = outer_len;
      return make_integer(0);
    }
    size_t pkg_end = pkg_start + pkglen;
    if (c->pos > pkg_end) {
      LOG_WARN("[AML] Buffer con PkgLength demasiado corto");
      c->pos = pkg_end;
      return make_integer(0);
    }
    // Limitar el parseo de BufferSize al propio paquete: nunca debe leer
    // bytes pertenecientes al siguiente TermObj si el paquete está truncado.
    c->len = pkg_end;
    aml_object_t *sz = parse_term_arg(c);
    uint64_t n = sz ? aml_to_integer(sz) : 0;
    size_t avail = c->pos < pkg_end ? pkg_end - c->pos : 0;
    if (n > avail)
      n = avail;
    aml_object_t *b = make_buffer(c->data + c->pos, (uint32_t)n);
    c->pos = pkg_end;
    c->len = outer_len;
    return b;
  }

  case AML_PACKAGE_OP:
  case AML_VAR_PACKAGE_OP: {
    c->pos++;
    size_t pkg_start = c->pos;
    size_t outer_len = c->len;
    uint32_t pkglen = decode_pkglength(c->data, outer_len, &c->pos);
    if (!pkglen || pkg_start > outer_len || pkglen > outer_len - pkg_start) {
      LOG_WARN("[AML] Package con PkgLength inválido");
      c->pos = outer_len;
      return make_integer(0);
    }
    size_t pkg_end = pkg_start + pkglen;
    if (c->pos > pkg_end) {
      LOG_WARN("[AML] Package con PkgLength demasiado corto");
      c->pos = pkg_end;
      return make_integer(0);
    }
    c->len = pkg_end;

    uint32_t nelem;
    if (op == AML_PACKAGE_OP) {
      if (c->pos >= pkg_end) {
        c->len = outer_len;
        c->pos = pkg_end;
        LOG_WARN("[AML] Package sin NumElements");
        return make_integer(0);
      }
      nelem = c->data[c->pos++];
    } else {
      aml_object_t *nv = parse_term_arg(c);
      nelem = nv ? (uint32_t)aml_to_integer(nv) : 0;
    }

    aml_object_t *pkg = make_package(nelem);
    if (!pkg) {
      c->pos = pkg_end;
      c->len = outer_len;
      return make_integer(0);
    }

    for (uint32_t i = 0; i < nelem && c->pos < pkg_end; i++) {
      size_t before = c->pos;
      pkg->package.elems[i] = parse_term_arg(c);
      if (!pkg->package.elems[i])
        pkg->package.elems[i] = make_integer(0);
      if (c->pos <= before) {
        LOG_WARN("[AML] Package: TermArg no avanzó; abortando paquete");
        break;
      }
    }
    c->pos = pkg_end;
    c->len = outer_len;
    return pkg;
  }

  default:
    // Referencia a nombre. Devolvemos el nodo si existe, o un
    // integer 0 si no.
    if ((op >= 'A' && op <= 'Z') || op == '_' || op == '\\' || op == '^') {
      char name[256];
      if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
        return make_integer(0);
      char resolved[512];
      resolve_name(c->scope, name, resolved, sizeof(resolved));
      aml_node_t *n = lookup_path_abs(resolved);
      if (n && n->value && n->value->type == AML_METHOD) {
        // A method invocation is NameString followed by ArgCount TermArgs.
        // Leaving those arguments unconsumed was the source of many bogus
        // "unknown opcode" warnings (e.g. ASCII bytes from string arguments).
        for (int i = 0; i < n->value->method.arg_count; ++i) {
          size_t before = c->pos;
          if (!aml_skip_term(c, c->len, 0) || c->pos <= before) {
            LOG_WARN("[AML] no se pudieron consumir argumentos del método %s",
                     name);
            c->pos = c->len;
            break;
          }
        }
        return n->value;
      }
      if (n && n->value)
        return n->value;
      return make_integer(0);
    }
    // [FIX CRÍTICO] Opcode desconocido en un data object.
    // Avanzamos 1 byte para evitar bucles infinitos o desincronización.
    LOG_WARN("[AML] parse_data_object: opcode 0x%02x desconocido, saltando",
             op);
    c->pos++;
    return make_integer(0);
  }
}

// Alias de parse_data_object. Reservado por si en el futuro cambia.
static aml_object_t *parse_term_arg(parse_ctx_t *c) {
  return parse_data_object(c);
}

// ===========================================================================
// FieldList (dentro de Field/IndexField/BankField)
// ===========================================================================
// FieldElement := NamedField | ReservedField | AccessField |
// ExtendedAccessField
//   NamedField    := NameSeg PkgLength
//   ReservedField := 0x00 PkgLength
//   AccessField   := 0x01 AccessType AccessAttrib
//   ConnectField  := 0x02 NameString | 0x02 BufferData
//   ExtendedAccess:= 0x03 AccessType ExtendedAccessAttrib AccessLength
static void parse_field_list(parse_ctx_t *c, size_t end_pos) {
  while (c->pos < end_pos && c->pos < c->len) {
    uint8_t b = c->data[c->pos];
    if (b == 0x00) {
      c->pos++;
      decode_pkglength(c->data, c->len, &c->pos);
    } else if (b == 0x01) {
      c->pos += 3; // AccessType + AccessAttrib
    } else if (b == 0x02) {
      c->pos++;
      if (c->pos < c->len && c->data[c->pos] == 0x11) {
        // BufferData
        parse_data_object(c);
      } else {
        char nm[256];
        read_namestring(c->data, c->len, &c->pos, nm, sizeof(nm));
      }
    } else if (b == 0x03) {
      c->pos += 4;
    } else {
      // NamedField: NameSeg + PkgLength
      if (c->pos + 4 > c->len)
        return;
      c->pos += 4;
      decode_pkglength(c->data, c->len, &c->pos);
    }
  }
  if (c->pos > end_pos)
    c->pos = end_pos;
}

// ===========================================================================
// TermList parser: declaraciones a nivel de scope
// ===========================================================================

// ---------------------------------------------------------------------------
// Calcula la longitud de un Predicate (el TermArg que va dentro de If/While)
// sin evaluarlo. Solo reconoce los patrones comunes:
//   - Nombre o método con args (ej: _OSI("..."))
//   - Comparaciones binarias: LEqual/LGreater/LLess/LAnd/LOr
//   - LNot
//   - Integers y prefijos de integer
// Devuelve la longitud en bytes desde `pos`, o 0 si no lo reconoce.
// ---------------------------------------------------------------------------
static size_t skip_predicate(const uint8_t *code, size_t len, size_t pos) {
  size_t start = pos;
  if (pos >= len)
    return 0;
  uint8_t op = code[pos];

  // Nombre simple o método con argumentos (típicamente _OSI).
  if ((op >= 'A' && op <= 'Z') || op == '_' || op == '\\' || op == '^') {
    char name[256];
    size_t p = pos;
    if (read_namestring(code, len, &p, name, sizeof(name)) < 0)
      return 0;
    // Consumir args primitivos (strings, integers) hasta que aparezca
    // un byte que no lo sea.
    int consumed = 0;
    while (p < len && consumed < 7) {
      uint8_t b = code[p];
      if (b == AML_STRING_PREFIX) {
        p++;
        while (p < len && code[p] != 0)
          p++;
        if (p < len)
          p++;
        consumed++;
      } else if (b == AML_ZERO_OP || b == AML_ONE_OP || b == AML_ONES_OP) {
        p++;
        consumed++;
      } else if (b == AML_BYTE_PREFIX) {
        p += 2;
        consumed++;
      } else if (b == AML_WORD_PREFIX) {
        p += 3;
        consumed++;
      } else if (b == AML_DWORD_PREFIX) {
        p += 5;
        consumed++;
      } else if (b == AML_QWORD_PREFIX) {
        p += 9;
        consumed++;
      } else {
        break;
      }
    }
    return p - start;
  }

  // Comparaciones binarias
  if (op == 0x90 || op == 0x91 || op == 0x93 || op == 0x94 || op == 0x95) {
    pos++;
    size_t a = skip_predicate(code, len, pos);
    if (!a)
      return 0;
    pos += a;
    size_t b = skip_predicate(code, len, pos);
    if (!b)
      return 0;
    pos += b;
    return pos - start;
  }

  // LNot
  if (op == 0x92) {
    pos++;
    size_t a = skip_predicate(code, len, pos);
    if (!a)
      return 0;
    return (pos + a) - start;
  }

  // Integers simples
  if (op == AML_ZERO_OP || op == AML_ONE_OP || op == AML_ONES_OP)
    return 1;
  if (op == AML_BYTE_PREFIX)
    return 2;
  if (op == AML_WORD_PREFIX)
    return 3;
  if (op == AML_DWORD_PREFIX)
    return 5;
  if (op == AML_QWORD_PREFIX)
    return 9;

  return 0;
}


// Consume AML executable terms without evaluating them. This is needed when
// parsing module-level If/While blocks: treating Store/Add/etc. as a data
// object advances one byte and makes operand bytes look like opcodes.
static int aml_skip_term(parse_ctx_t *c, size_t limit, unsigned depth);

static int aml_skip_supername(parse_ctx_t *c, size_t limit, unsigned depth) {
  if (c->pos >= limit || c->pos >= c->len || depth > 32) return 0;
  uint8_t op = c->data[c->pos];
  if ((op >= AML_LOCAL0_OP && op <= AML_LOCAL7_OP) ||
      (op >= AML_ARG0_OP && op <= AML_ARG6_OP) || op == 0x00) {
    c->pos++;
    return 1;
  }
  if (op == AML_EXT_OP_PREFIX && c->pos + 1 < limit &&
      c->data[c->pos + 1] == AML_EXT_DEBUG) {
    c->pos += 2;
    return 1;
  }
  if ((op >= 'A' && op <= 'Z') || op == '_' || op == '\\' || op == '^') {
    char nm[256];
    return read_namestring(c->data, limit, &c->pos, nm, sizeof(nm)) >= 0;
  }
  return aml_skip_term(c, limit, depth + 1);
}

static int aml_skip_term(parse_ctx_t *c, size_t limit, unsigned depth) {
  if (c->pos >= limit || c->pos >= c->len || depth > 32) return 0;
  size_t start = c->pos;
  uint8_t op = c->data[c->pos];

  if (op == AML_ZERO_OP || op == AML_ONE_OP || op == AML_ONES_OP) {
    c->pos++; return 1;
  }
  if (op == AML_BYTE_PREFIX || op == AML_WORD_PREFIX ||
      op == AML_DWORD_PREFIX || op == AML_QWORD_PREFIX) {
    size_t n = op == AML_BYTE_PREFIX ? 2 : op == AML_WORD_PREFIX ? 3 :
               op == AML_DWORD_PREFIX ? 5 : 9;
    if (start + n > limit || start + n > c->len) return 0;
    c->pos += n; return 1;
  }
  if (op == AML_STRING_PREFIX) {
    c->pos++;
    while (c->pos < limit && c->pos < c->len && c->data[c->pos]) c->pos++;
    if (c->pos < limit && c->pos < c->len) c->pos++;
    return 1;
  }
  if (op == AML_BUFFER_OP || op == AML_PACKAGE_OP || op == AML_VAR_PACKAGE_OP) {
    c->pos++;
    size_t pkg_start = c->pos;
    uint32_t pkglen = decode_pkglength(c->data, limit, &c->pos);
    if (!pkglen || pkg_start > limit || pkglen > limit - pkg_start) return 0;
    c->pos = pkg_start + pkglen; return 1;
  }

  if ((op >= 'A' && op <= 'Z') || op == '_' || op == '\\' || op == '^') {
    char nm[256];
    if (read_namestring(c->data, limit, &c->pos, nm, sizeof(nm)) < 0) return 0;
    char resolved[512];
    resolve_name(c->scope, nm, resolved, sizeof(resolved));
    aml_node_t *n = lookup_path_abs(resolved);
    if (n && n->value && n->value->type == AML_METHOD) {
      for (int i = 0; i < n->value->method.arg_count; ++i)
        if (!aml_skip_term(c, limit, depth + 1)) return 0;
    }
    return c->pos > start;
  }

  switch (op) {
  case AML_STORE_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1);
  case AML_REF_OF_OP: case AML_INCREMENT_OP: case AML_DECREMENT_OP:
  case AML_SIZE_OF_OP:
    c->pos++; return aml_skip_supername(c, limit, depth + 1);
  case AML_ADD_OP: case AML_CONCAT_OP: case AML_SUBTRACT_OP:
  case AML_MULTIPLY_OP: case AML_SHIFT_LEFT_OP: case AML_SHIFT_RIGHT_OP:
  case AML_AND_OP: case AML_NAND_OP: case AML_OR_OP: case AML_NOR_OP:
  case AML_XOR_OP: case AML_MOD_OP: case AML_CONCAT_RES_OP: case AML_INDEX_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_term(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1);
  case AML_DIVIDE_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_term(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1);
  case AML_NOT_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1);
  case AML_LNOT_OP:
    c->pos++; return aml_skip_term(c, limit, depth + 1);
  case AML_LAND_OP: case AML_LOR_OP: case AML_LEQUAL_OP:
  case AML_LGREATER_OP: case AML_LLESS_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_term(c, limit, depth + 1);
  case AML_FIND_SET_LEFT: case AML_FIND_SET_RIGHT:
  case AML_TO_BUFFER_OP: case AML_TO_DECSTRING_OP: case AML_TO_HEXSTRING_OP:
  case AML_TO_INTEGER_OP: case AML_COPY_OBJECT_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1);
  case AML_TO_STRING_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_term(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1);
  case AML_MID_OP:
    c->pos++;
    return aml_skip_term(c, limit, depth + 1) &&
           aml_skip_term(c, limit, depth + 1) &&
           aml_skip_term(c, limit, depth + 1) &&
           aml_skip_supername(c, limit, depth + 1);
  case AML_RETURN_OP:
    c->pos++; return aml_skip_term(c, limit, depth + 1);
  case AML_NOTIFY_OP:
    c->pos++;
    return aml_skip_supername(c, limit, depth + 1) &&
           aml_skip_term(c, limit, depth + 1);
  case AML_CONTINUE_OP: case AML_BREAK_OP: case AML_NOOP_OP:
  case AML_BREAKPOINT_OP:
    c->pos++; return 1;
  case AML_EXT_OP_PREFIX: {
    c->pos++;
    if (c->pos >= limit) return 0;
    uint8_t ext = c->data[c->pos++];
    switch (ext) {
    case AML_EXT_STALL: case AML_EXT_SLEEP:
      return aml_skip_term(c, limit, depth + 1);
    case AML_EXT_SIGNAL: case AML_EXT_RESET: case AML_EXT_RELEASE:
      return aml_skip_supername(c, limit, depth + 1);
    case AML_EXT_WAIT:
      return aml_skip_supername(c, limit, depth + 1) &&
             aml_skip_term(c, limit, depth + 1);
    case AML_EXT_ACQUIRE:
      if (!aml_skip_supername(c, limit, depth + 1) || c->pos + 2 > limit)
        return 0;
      c->pos += 2; return 1;
    case AML_EXT_REVISION: case AML_EXT_DEBUG: case AML_EXT_TIMER:
      return 1;
    default: return 0;
    }
  }
  default:
    return 0;
  }
}

static void parse_term_list(parse_ctx_t *c, size_t end_pos) {
  int guard = 0;
  while (c->pos < end_pos && c->pos < c->len) {
    if (++guard > 100000)
      return;
    uint8_t op = c->data[c->pos];

    switch (op) {
    case AML_NAME_OP: {
      c->pos++;
      char name[256];
      if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
        return;
      char resolved[512];
      resolve_name(c->scope, name, resolved, sizeof(resolved));
      aml_node_t *n = ensure_path(resolved);

      size_t before = c->pos;
      aml_object_t *v = parse_term_arg(c);
      size_t after = c->pos;

      if (after < before || after > end_pos) {
        LOG_WARN(
            "[AML] Name('%s') desbordó: before=0x%zx after=0x%zx end=0x%zx",
            name, before, after, end_pos);
        c->pos = end_pos;
        return;
      }

      if (n && v)
        n->value = v;
      break;
    }

    case AML_SCOPE_OP: {
      c->pos++;
      size_t pkg_start = c->pos;
      uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
      size_t scope_end = pkg_start + pkglen;

      char name[256];
      if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
        return;
      char resolved[512];
      resolve_name(c->scope, name, resolved, sizeof(resolved));

      aml_node_t *old_scope = c->scope;
      aml_node_t *n = ensure_path(resolved);
      if (n)
        c->scope = n;
      parse_term_list(c, scope_end);
      c->scope = old_scope;
      c->pos = scope_end;
      break;
    }

    case AML_METHOD_OP: {
      c->pos++;
      size_t pkg_start = c->pos;
      uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
      size_t body_end = pkg_start + pkglen;

      char name[256];
      if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
        return;
      if (c->pos >= body_end) {
        c->pos = body_end;
        break;
      }
      uint8_t flags = c->data[c->pos++];

      char resolved[512];
      resolve_name(c->scope, name, resolved, sizeof(resolved));
      aml_node_t *n = ensure_path(resolved);
      if (n) {
        aml_object_t *m = alloc_obj(AML_METHOD);
        if (m) {
          m->method.code = (uint8_t *)(c->data + c->pos);
          m->method.code_len = (uint32_t)(body_end - c->pos);
          m->method.arg_count = flags & 0x07;
          m->method.flags = flags;
          m->method.scope = n;
          n->value = m;
        }
      }
      c->pos = body_end;
      break;
    }

    case AML_EXT_OP_PREFIX: {
      c->pos++;
      if (c->pos >= c->len)
        return;
      uint8_t ext = c->data[c->pos++];

      switch (ext) {
      case AML_EXT_DEVICE:
      case AML_EXT_THERMALZONE: {
        size_t pkg_start = c->pos;
        uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
        size_t obj_end = pkg_start + pkglen;
        char name[256];
        if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
          return;
        char resolved[512];
        resolve_name(c->scope, name, resolved, sizeof(resolved));
        aml_node_t *old_scope = c->scope;
        aml_node_t *n = ensure_path(resolved);
        if (n) {
          n->value =
              alloc_obj(ext == AML_EXT_DEVICE ? AML_DEVICE : AML_THERMALZONE);
          c->scope = n;
        }
        parse_term_list(c, obj_end);
        c->scope = old_scope;
        c->pos = obj_end;
        break;
      }

      case AML_EXT_PROCESSOR: {
        size_t pkg_start = c->pos;
        uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
        size_t obj_end = pkg_start + pkglen;
        char name[256];
        if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
          return;
        if (c->pos + 6 > obj_end) {
          c->pos = obj_end;
          break;
        }
        c->pos += 6; // ProcID (1) + PblkAddr (4) + PblkLen (1)
        char resolved[512];
        resolve_name(c->scope, name, resolved, sizeof(resolved));
        aml_node_t *old_scope = c->scope;
        aml_node_t *n = ensure_path(resolved);
        if (n) {
          n->value = alloc_obj(AML_PROCESSOR);
          c->scope = n;
        }
        parse_term_list(c, obj_end);
        c->scope = old_scope;
        c->pos = obj_end;
        break;
      }

      case AML_EXT_POWERRES: {
        size_t pkg_start = c->pos;
        uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
        size_t obj_end = pkg_start + pkglen;
        char name[256];
        if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
          return;
        if (c->pos + 2 <= obj_end)
          c->pos += 2; // SystemLevel + ResourceOrder
        char resolved[512];
        resolve_name(c->scope, name, resolved, sizeof(resolved));
        aml_node_t *old_scope = c->scope;
        aml_node_t *n = ensure_path(resolved);
        if (n) {
          n->value = alloc_obj(AML_POWERRES);
          c->scope = n;
        }
        parse_term_list(c, obj_end);
        c->scope = old_scope;
        c->pos = obj_end;
        break;
      }

      case AML_EXT_OPREGION: {
        // DefOpRegion := ExtOpPrefix 0x80 NameString RegionSpace RegionOffset
        // RegionLen. No lleva PkgLength (a diferencia de Field/Device/...).
        char name[256];
        if (read_namestring(c->data, c->len, &c->pos, name, sizeof(name)) < 0)
          return;
        if (c->pos >= c->len)
          break;
        uint8_t space = c->data[c->pos++];
        aml_object_t *off = parse_term_arg(c);
        aml_object_t *len = parse_term_arg(c);

        char resolved[512];
        resolve_name(c->scope, name, resolved, sizeof(resolved));
        aml_node_t *n = ensure_path(resolved);
        if (n) {
          aml_object_t *o = alloc_obj(AML_OPREGION);
          if (o) {
            o->opregion.base = off ? aml_to_integer(off) : 0;
            o->opregion.len = len ? (uint32_t)aml_to_integer(len) : 0;
            o->opregion.buf = (space == 0) ? (uint8_t *)o->opregion.base : NULL;
            n->value = o;
          }
        }
        break;
      }

      case AML_EXT_FIELD: {
        size_t pkg_start = c->pos;
        uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
        size_t obj_end = pkg_start + pkglen;
        char rname[256];
        read_namestring(c->data, c->len, &c->pos, rname, sizeof(rname));
        if (c->pos < obj_end)
          c->pos++; // FieldFlags
        parse_field_list(c, obj_end);
        c->pos = obj_end;
        break;
      }

      case AML_EXT_INDEX_FIELD:
      case AML_EXT_BANK_FIELD: {
        size_t pkg_start = c->pos;
        uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
        size_t obj_end = pkg_start + pkglen;
        char rname[256];
        read_namestring(c->data, c->len, &c->pos, rname, sizeof(rname));
        read_namestring(c->data, c->len, &c->pos, rname, sizeof(rname));
        if (ext == AML_EXT_BANK_FIELD) {
          parse_term_arg(c);
          read_namestring(c->data, c->len, &c->pos, rname, sizeof(rname));
        }
        if (c->pos < obj_end)
          c->pos++; // FieldFlags
        parse_field_list(c, obj_end);
        c->pos = obj_end;
        break;
      }

      case AML_EXT_MUTEX_OP: {
        char nm[256];
        read_namestring(c->data, c->len, &c->pos, nm, sizeof(nm));
        c->pos++;
        char resolved[512];
        resolve_name(c->scope, nm, resolved, sizeof(resolved));
        aml_node_t *n = ensure_path(resolved);
        if (n && !n->value)
          n->value = alloc_obj(AML_MUTEX);
        break;
      }

      case AML_EXT_EVENT_OP: {
        char nm[256];
        read_namestring(c->data, c->len, &c->pos, nm, sizeof(nm));
        char resolved[512];
        resolve_name(c->scope, nm, resolved, sizeof(resolved));
        aml_node_t *n = ensure_path(resolved);
        if (n && !n->value)
          n->value = alloc_obj(AML_EVENT);
        break;
      }

      default:
        // [FIX] Opcode extendido desconocido.
        // Antes hacíamos 'return' y abortábamos el SSDT entero.
        // Ahora intentamos saltar el paquete asociado y seguir.
        LOG_WARN(
            "[AML] Opcode ext desconocido 0x5B 0x%02x, intentando saltar...",
            ext);
        if (c->pos < c->len) {
          size_t before = c->pos;
          decode_pkglength(c->data, c->len, &c->pos);
          if (c->pos == before)
            c->pos++; // Si no era un pkglen, saltamos 1 byte
        }
        break;
      }
      break;
    }

    case AML_EXTERNAL_OP: {
      c->pos++;
      char nm[256];
      if (read_namestring(c->data, c->len, &c->pos, nm, sizeof(nm)) < 0)
        return;
      if (c->pos + 2 <= end_pos)
        c->pos += 2; // Type + ArgCount
      break;
    }

    case AML_ALIAS_OP: {
      c->pos++;
      char a[256], b[256];
      read_namestring(c->data, c->len, &c->pos, a, sizeof(a));
      read_namestring(c->data, c->len, &c->pos, b, sizeof(b));
      break;
    }

    case AML_IF_OP:
    case AML_WHILE_OP: {
      c->pos++;
      size_t pkg_start = c->pos;
      uint32_t pkglen = decode_pkglength(c->data, c->len, &c->pos);
      size_t pkg_end = pkg_start + pkglen;

      size_t pred_start = c->pos;
      size_t pred_len = skip_predicate(c->data, c->len, pred_start);
      if (pred_len == 0) {
        c->pos = pkg_end;
      } else {
        c->pos = pred_start + pred_len;
        parse_term_list(c, pkg_end);
        c->pos = pkg_end;
      }

      if (op == AML_IF_OP && c->pos < end_pos &&
          c->data[c->pos] == AML_ELSE_OP) {
        c->pos++;
        size_t ep_start = c->pos;
        uint32_t eplen = decode_pkglength(c->data, c->len, &c->pos);
        size_t ep_end = ep_start + eplen;
        parse_term_list(c, ep_end);
        c->pos = ep_end;
      }
      break;
    }

    case AML_NOOP_OP:
      c->pos++;
      break;

    default: {
      // Los bloques If/While pueden contener TermOps ejecutables. No
      // pasarlos por parse_data_object: no son DataRefObjects.
      size_t before = c->pos;
      if (!aml_skip_term(c, end_pos, 0) || c->pos <= before ||
          c->pos > end_pos) {
        LOG_WARN("[AML] Scope: término AML no soportado 0x%02x @ 0x%zx; "
                 "se omite el resto del paquete",
                 op, before);
        c->pos = end_pos;
      }
      break;
    }
    }
  }
  c->pos = end_pos;
}

// ===========================================================================
// Intérprete
// ===========================================================================
typedef struct local_var {
  char name[5];
  aml_object_t *val;
  struct local_var *next;
} local_var_t;

typedef struct eval_ctx {
  aml_object_t *locals[AML_MAX_LOCALS];
  aml_object_t *args[AML_MAX_ARGS];
  local_var_t *method_locals; // variables locales creadas con Name()
  aml_node_t *scope;
  int return_set;
  int break_set;
  int continue_set;
  int depth;
  aml_object_t *ret_val;
} eval_ctx_t;

static void local_set(eval_ctx_t *e, const char *name, aml_object_t *v) {
  local_var_t *lv = e->method_locals;
  while (lv) {
    if (memcmp(lv->name, name, 4) == 0) {
      lv->val = v;
      return;
    }
    lv = lv->next;
  }
  lv = (local_var_t *)kmalloc(sizeof(local_var_t));
  if (!lv)
    return;
  memcpy(lv->name, name, 4);
  lv->name[4] = '\0';
  lv->val = v;
  lv->next = e->method_locals;
  e->method_locals = lv;
}

static aml_object_t *local_get(eval_ctx_t *e, const char *name) {
  for (local_var_t *lv = e->method_locals; lv; lv = lv->next)
    if (memcmp(lv->name, name, 4) == 0)
      return lv->val;
  return NULL;
}

static aml_object_t *eval_term(eval_ctx_t *e, const uint8_t *code,
                               size_t code_len, size_t *pos);

static aml_object_t *eval_method(aml_object_t *m, aml_object_t **args,
                                 int nargs, int depth);

// Resuelve un NameString dentro del scope actual, consultando primero
// las locales del método y luego el namespace global.
static aml_object_t *resolve_and_read(eval_ctx_t *e, const char *name) {
  // ¿Es un Local0-7 o Arg0-6? Se manejan en eval_term directamente, no
  // aquí.
  // Buscar en locales del método primero.
  if (!(name[0] == '\\' || name[0] == '^')) {
    // Nombre simple (4 chars). Buscar en locals.
    if (name[4] == '\0') {
      aml_object_t *lv = local_get(e, name);
      if (lv)
        return lv;
    }
  }
  char resolved[512];
  resolve_name(e->scope, name, resolved, sizeof(resolved));
  aml_node_t *n = lookup_path_abs(resolved);
  if (!n || !n->value)
    return NULL;
  return n->value;
}

// Parser de un campo Index (para Store a un buffer local). Usamos una
// representación simple: una referencia a un buffer + offset.
typedef struct {
  aml_object_t *target;
  uint64_t index;
} aml_ref_t;

// Dado un término, si es un Index() reciente, devolvemos el objeto
// "referenciado". Simplificación: si target es buffer, indexamos bytes.
// Los Store a buffer se manejan aparte en eval_term.
static aml_object_t *deref(aml_object_t *o) { return o; }

// Evalúa un término. Devuelve:
//   - aml_object_t*  valor
//   - NULL           si el término no produjo valor (p.ej. Store)
static aml_object_t *eval_term(eval_ctx_t *e, const uint8_t *code,
                               size_t code_len, size_t *pos) {
  if (*pos >= code_len)
    return NULL;
  if (e->depth > AML_MAX_DEPTH)
    return make_integer(0);

  uint8_t op = code[(*pos)++];
  e->depth++;

  aml_object_t *ret = NULL;

  // ---- Locals/Args ----
  if (op >= AML_LOCAL0_OP && op <= AML_LOCAL7_OP) {
    int idx = op - AML_LOCAL0_OP;
    ret = e->locals[idx];
    if (!ret)
      ret = make_integer(0);
    goto done;
  }
  if (op >= AML_ARG0_OP && op <= AML_ARG6_OP) {
    int idx = op - AML_ARG0_OP;
    ret = e->args[idx];
    if (!ret)
      ret = make_integer(0);
    goto done;
  }

  switch (op) {
  case AML_ZERO_OP:
    ret = make_integer(0);
    break;
  case AML_ONE_OP:
    ret = make_integer(1);
    break;
  case AML_ONES_OP:
    ret = make_integer(0xFFFFFFFFFFFFFFFFULL);
    break;

  case AML_BYTE_PREFIX:
    if (*pos < code_len)
      ret = make_integer(code[(*pos)++]);
    else
      ret = make_integer(0);
    break;

  case AML_WORD_PREFIX: {
    uint64_t v = code[*pos] | ((uint64_t)code[*pos + 1] << 8);
    *pos += 2;
    ret = make_integer(v);
    break;
  }
  case AML_DWORD_PREFIX: {
    uint64_t v = code[*pos] | ((uint64_t)code[*pos + 1] << 8) |
                 ((uint64_t)code[*pos + 2] << 16) |
                 ((uint64_t)code[*pos + 3] << 24);
    *pos += 4;
    ret = make_integer(v);
    break;
  }
  case AML_QWORD_PREFIX: {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
      v |= ((uint64_t)code[*pos + i]) << (i * 8);
    *pos += 8;
    ret = make_integer(v);
    break;
  }
  case AML_STRING_PREFIX: {
    size_t start = *pos;
    while (*pos < code_len && code[*pos] != 0)
      (*pos)++;
    size_t n = *pos - start;
    if (*pos < code_len)
      (*pos)++;
    ret = make_string((const char *)(code + start), (uint32_t)n);
    break;
  }

  case AML_RETURN_OP: {
    aml_object_t *v = eval_term(e, code, code_len, pos);
    e->return_set = 1;
    e->ret_val = v ? v : make_integer(0);
    ret = e->ret_val;
    break;
  }

  case AML_STORE_OP: {
    // Orden: TermArg (valor) + SuperName (target)
    aml_object_t *v = eval_term(e, code, code_len, pos);
    // Leer el target
    if (*pos >= code_len)
      break;
    uint8_t tk = code[*pos];
    if (tk >= AML_LOCAL0_OP && tk <= AML_LOCAL7_OP) {
      (*pos)++;
      e->locals[tk - AML_LOCAL0_OP] = v;
    } else if (tk >= AML_ARG0_OP && tk <= AML_ARG6_OP) {
      (*pos)++;
      e->args[tk - AML_ARG0_OP] = v;
    } else if ((tk >= 'A' && tk <= 'Z') || tk == '_' || tk == '\\' ||
               tk == '^') {
      char nm[256];
      read_namestring(code, code_len, pos, nm, sizeof(nm));
      if (nm[4] == '\0' && !(nm[0] == '\\' || nm[0] == '^')) {
        local_set(e, nm, v);
      } else {
        char resolved[512];
        resolve_name(e->scope, nm, resolved, sizeof(resolved));
        aml_node_t *n = lookup_path_abs(resolved);
        if (n)
          n->value = v;
      }
    } else {
      // Otro tipo (Index, etc.). Consumir y descartar.
      eval_term(e, code, code_len, pos);
    }
    ret = v;
    break;
  }

  case AML_COPY_OBJECT_OP: {
    aml_object_t *v = eval_term(e, code, code_len, pos);
    uint8_t tk = code[(*pos)++];
    if (tk >= AML_LOCAL0_OP && tk <= AML_LOCAL7_OP) {
      e->locals[tk - AML_LOCAL0_OP] = v;
    } else if ((tk >= 'A' && tk <= 'Z') || tk == '_') {
      // Consumido en la rama anterior por namestring.
      (*pos)--;
      char nm[256];
      read_namestring(code, code_len, pos, nm, sizeof(nm));
      if (nm[4] == '\0')
        local_set(e, nm, v);
    }
    ret = v;
    break;
  }

  case AML_ADD_OP:
  case AML_SUBTRACT_OP:
  case AML_MULTIPLY_OP:
  case AML_DIVIDE_OP:
  case AML_MOD_OP:
  case AML_SHIFT_LEFT_OP:
  case AML_SHIFT_RIGHT_OP:
  case AML_AND_OP:
  case AML_OR_OP:
  case AML_XOR_OP:
  case AML_NAND_OP:
  case AML_NOR_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    aml_object_t *b = eval_term(e, code, code_len, pos);
    uint64_t av = a ? aml_to_integer(a) : 0;
    uint64_t bv = b ? aml_to_integer(b) : 0;
    uint64_t r = 0;
    switch (op) {
    case AML_ADD_OP:
      r = av + bv;
      break;
    case AML_SUBTRACT_OP:
      r = av - bv;
      break;
    case AML_MULTIPLY_OP:
      r = av * bv;
      break;
    case AML_DIVIDE_OP:
      r = bv ? av / bv : 0;
      break;
    case AML_MOD_OP:
      r = bv ? av % bv : 0;
      break;
    case AML_SHIFT_LEFT_OP:
      r = av << (bv & 63);
      break;
    case AML_SHIFT_RIGHT_OP:
      r = av >> (bv & 63);
      break;
    case AML_AND_OP:
      r = av & bv;
      break;
    case AML_OR_OP:
      r = av | bv;
      break;
    case AML_XOR_OP:
      r = av ^ bv;
      break;
    case AML_NAND_OP:
      r = ~(av & bv);
      break;
    case AML_NOR_OP:
      r = ~(av | bv);
      break;
    }
    // Tercer argumento: Target (opcional)
    if (*pos < code_len) {
      uint8_t tk = code[*pos];
      if (tk >= AML_LOCAL0_OP && tk <= AML_LOCAL7_OP) {
        (*pos)++;
        e->locals[tk - AML_LOCAL0_OP] = make_integer(r);
      } else if (tk == AML_DIVIDE_OP || tk == AML_MOD_OP) {
        // no aplica
      } else if ((tk >= 'A' && tk <= 'Z') || tk == '_' || tk == '\\' ||
                 tk == '^') {
        char nm[256];
        read_namestring(code, code_len, pos, nm, sizeof(nm));
        if (nm[4] == '\0' && nm[0] != '\\' && nm[0] != '^') {
          local_set(e, nm, make_integer(r));
        }
      }
    }
    ret = make_integer(r);
    break;
  }

  case AML_NOT_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    uint64_t av = a ? aml_to_integer(a) : 0;
    uint64_t r = ~av;
    if (*pos < code_len) {
      uint8_t tk = code[*pos];
      if (tk >= AML_LOCAL0_OP && tk <= AML_LOCAL7_OP) {
        (*pos)++;
        e->locals[tk - AML_LOCAL0_OP] = make_integer(r);
      } else if ((tk >= 'A' && tk <= 'Z') || tk == '_') {
        char nm[256];
        read_namestring(code, code_len, pos, nm, sizeof(nm));
        if (nm[4] == '\0')
          local_set(e, nm, make_integer(r));
      }
    }
    ret = make_integer(r);
    break;
  }

  case AML_LNOT_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    uint64_t av = a ? aml_to_integer(a) : 0;
    ret = make_integer(av == 0 ? 1 : 0);
    break;
  }

  case AML_LEQUAL_OP:
  case AML_LGREATER_OP:
  case AML_LLESS_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    aml_object_t *b = eval_term(e, code, code_len, pos);
    uint64_t av = a ? aml_to_integer(a) : 0;
    uint64_t bv = b ? aml_to_integer(b) : 0;
    uint64_t r;
    if (op == AML_LEQUAL_OP)
      r = (av == bv) ? 1 : 0;
    else if (op == AML_LGREATER_OP)
      r = (av > bv) ? 1 : 0;
    else
      r = (av < bv) ? 1 : 0;
    ret = make_integer(r);
    break;
  }

  case AML_LAND_OP:
  case AML_LOR_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    uint64_t av = a ? aml_to_integer(a) : 0;
    if (op == AML_LAND_OP && av == 0) {
      ret = make_integer(0);
    } else if (op == AML_LOR_OP && av != 0) {
      ret = make_integer(1);
    } else {
      aml_object_t *b = eval_term(e, code, code_len, pos);
      uint64_t bv = b ? aml_to_integer(b) : 0;
      ret = make_integer(bv != 0 ? 1 : 0);
    }
    break;
  }

  case AML_SIZE_OF_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    uint64_t n = 0;
    if (a) {
      if (a->type == AML_BUFFER)
        n = a->buffer.len;
      else if (a->type == AML_STRING)
        n = a->string.len;
      else if (a->type == AML_PACKAGE)
        n = a->package.count;
    }
    ret = make_integer(n);
    break;
  }

  case AML_TO_INTEGER_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    ret = make_integer(a ? aml_to_integer(a) : 0);
    break;
  }

  case AML_TO_BUFFER_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    uint64_t n = a ? aml_to_integer(a) : 0;
    (void)n;
    if (a && a->type == AML_BUFFER)
      ret = a;
    else {
      uint8_t b = (uint8_t)(a ? aml_to_integer(a) : 0);
      ret = make_buffer(&b, 1);
    }
    break;
  }

  case AML_TO_STRING_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    (void)a;
    ret = make_string("", 0);
    break;
  }

  case AML_TO_DECSTRING_OP:
  case AML_TO_HEXSTRING_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    uint64_t v = a ? aml_to_integer(a) : 0;
    char buf[24];
    int n = 0;
    if (op == AML_TO_DECSTRING_OP) {
      if (v == 0)
        buf[n++] = '0';
      else {
        char tmp[24];
        int t = 0;
        while (v) {
          tmp[t++] = (char)('0' + (v % 10));
          v /= 10;
        }
        while (t > 0)
          buf[n++] = tmp[--t];
      }
    } else {
      static const char hx[] = "0123456789ABCDEF";
      char tmp[24];
      int t = 0;
      if (v == 0)
        tmp[t++] = '0';
      while (v) {
        tmp[t++] = hx[v & 0xF];
        v >>= 4;
      }
      while (t > 0)
        buf[n++] = tmp[--t];
    }
    ret = make_string(buf, (uint32_t)n);
    break;
  }

  case AML_CONCAT_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    aml_object_t *b = eval_term(e, code, code_len, pos);
    if (a && b && a->type == AML_STRING && b->type == AML_STRING) {
      uint32_t n = a->string.len + b->string.len;
      char *s = (char *)kmalloc(n + 1);
      if (s) {
        memcpy(s, a->string.data, a->string.len);
        memcpy(s + a->string.len, b->string.data, b->string.len);
        s[n] = '\0';
        ret = make_string(s, n);
        kfree(s);
      }
    } else {
      uint64_t av = a ? aml_to_integer(a) : 0;
      uint64_t bv = b ? aml_to_integer(b) : 0;
      uint8_t buf[16];
      int i = 0;
      for (int k = 0; k < 8; k++)
        buf[i++] = (uint8_t)(av >> (k * 8));
      for (int k = 0; k < 8; k++)
        buf[i++] = (uint8_t)(bv >> (k * 8));
      ret = make_buffer(buf, 16);
    }
    if (*pos < code_len) {
      uint8_t tk = code[*pos];
      if (tk >= AML_LOCAL0_OP && tk <= AML_LOCAL7_OP) {
        (*pos)++;
        e->locals[tk - AML_LOCAL0_OP] = ret;
      }
    }
    break;
  }

  case AML_MID_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    aml_object_t *idx = eval_term(e, code, code_len, pos);
    aml_object_t *len = eval_term(e, code, code_len, pos);
    uint64_t i = idx ? aml_to_integer(idx) : 0;
    uint64_t l = len ? aml_to_integer(len) : 0;
    if (a && a->type == AML_BUFFER) {
      if (i > a->buffer.len)
        i = a->buffer.len;
      uint64_t avail = a->buffer.len - i;
      if (l > avail)
        l = avail;
      ret = make_buffer(a->buffer.data + i, (uint32_t)l);
    } else if (a && a->type == AML_STRING) {
      if (i > a->string.len)
        i = a->string.len;
      uint64_t avail = a->string.len - i;
      if (l > avail)
        l = avail;
      ret = make_string(a->string.data + i, (uint32_t)l);
    } else {
      ret = make_integer(0);
    }
    break;
  }

  case AML_INDEX_OP: {
    // Index (Object, IndexValue) → Reference. Lo simplificamos:
    // devolvemos un package [obj, idx] que Store sabe manejar.
    aml_object_t *a = eval_term(e, code, code_len, pos);
    aml_object_t *b = eval_term(e, code, code_len, pos);
    aml_object_t *pkg = make_package(2);
    if (pkg) {
      pkg->package.elems[0] = a ? a : make_integer(0);
      pkg->package.elems[1] = b ? b : make_integer(0);
    }
    ret = pkg;
    break;
  }

  case AML_REF_OF_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    ret = a;
    break;
  }

  case AML_DEREF_OF_OP: {
    aml_object_t *a = eval_term(e, code, code_len, pos);
    ret = deref(a);
    break;
  }

  case AML_IF_OP: {
    // PkgLength viene PRIMERO, luego Predicate, luego TermList.
    size_t pkg_start = *pos;
    uint32_t pkglen = decode_pkglength(code, code_len, pos);
    size_t pkg_end = pkg_start + pkglen;

    aml_object_t *cond = eval_term(e, code, code_len, pos);
    uint64_t c = cond ? aml_to_integer(cond) : 0;

    if (c) {
      while (*pos < pkg_end) {
        eval_term(e, code, code_len, pos);
        if (e->return_set || e->break_set)
          break;
      }
      *pos = pkg_end;
    } else {
      *pos = pkg_end;
    }

    // ¿Else?
    if (*pos < code_len && code[*pos] == AML_ELSE_OP) {
      (*pos)++;
      size_t ep_start = *pos;
      uint32_t eplen = decode_pkglength(code, code_len, pos);
      size_t ep_end = ep_start + eplen;
      if (!c) {
        while (*pos < ep_end) {
          eval_term(e, code, code_len, pos);
          if (e->return_set || e->break_set)
            break;
        }
      }
      *pos = ep_end;
    }
    break;
  }

  case AML_WHILE_OP: {
    // PkgLength + Predicate + TermList
    size_t pkg_start = *pos;
    uint32_t pkglen = decode_pkglength(code, code_len, pos);
    size_t pkg_end = pkg_start + pkglen;
    size_t predicate_pos = *pos;

    int iter = 0;
    for (;;) {
      if (++iter > AML_WHILE_MAX_ITER) {
        LOG_WARN("[AML] While sin converger tras %d iteraciones", iter);
        break;
      }
      *pos = predicate_pos;
      aml_object_t *cond = eval_term(e, code, code_len, pos);
      uint64_t c = cond ? aml_to_integer(cond) : 0;
      if (!c) {
        *pos = pkg_end;
        break;
      }

      while (*pos < pkg_end) {
        eval_term(e, code, code_len, pos);
        if (e->return_set) {
          *pos = pkg_end;
          return e->ret_val;
        }
        if (e->break_set) {
          e->break_set = 0;
          *pos = pkg_end;
          break;
        }
        if (e->continue_set) {
          e->continue_set = 0;
          break;
        }
      }
      if (*pos >= pkg_end)
        *pos = pkg_end;
    }
    break;
  }

  case AML_BREAK_OP:
    e->break_set = 1;
    break;

  case AML_CONTINUE_OP:
    e->continue_set = 1;
    break;

  case AML_NOOP_OP:
  case AML_BREAKPOINT_OP:
    break;

  case AML_NOTIFY_OP: {
    // Notify (Object, Value). Consumimos los args y no hacemos nada.
    eval_term(e, code, code_len, pos);
    eval_term(e, code, code_len, pos);
    break;
  }

  case AML_EXT_OP_PREFIX: {
    if (op == AML_EXT_OP_PREFIX) {
      uint8_t ext = code[(*pos)++];
      switch (ext) {
      case AML_EXT_MUTEX_OP: {
        // No hacer nada, pero consumir los args si los hay.
        // (Aquí se llama a Acquire, no a definir Mutex.)
        break;
      }
      case AML_EXT_RELEASE: {
        if (*pos < code_len)
          (*pos)++; // SuperName? normalmente Local
        break;
      }
      case AML_EXT_ACQUIRE: {
        eval_term(e, code, code_len, pos); // Mutex
        if (*pos < code_len)
          (*pos)++; // Timeout
        break;
      }
      case AML_EXT_STALL: {
        eval_term(e, code, code_len, pos);
        break;
      }
      case AML_EXT_SLEEP: {
        eval_term(e, code, code_len, pos);
        break;
      }
      case AML_EXT_REVISION: {
        ret = make_integer(2); // ACPI 2.0
        break;
      }
      case AML_EXT_TIMER: {
        ret = make_integer(0);
        break;
      }
      case AML_EXT_FATAL: {
        // Fatal no debería aparecer en métodos evaluables.
        break;
      }
      case AML_EXT_COND_REF_OF: {
        // CondRefOf (Object, Target). Devolvemos 1 si existe.
        aml_object_t *a = eval_term(e, code, code_len, pos);
        ret = make_integer(a != NULL ? 1 : 0);
        // Consumir el Target (un SuperName).
        if (*pos < code_len) {
          uint8_t tk = code[*pos];
          if (tk >= AML_LOCAL0_OP && tk <= AML_LOCAL7_OP) {
            (*pos)++;
            e->locals[tk - AML_LOCAL0_OP] = ret;
          } else if ((tk >= 'A' && tk <= 'Z') || tk == '_' || tk == '\\' ||
                     tk == '^') {
            char nm[256];
            read_namestring(code, code_len, pos, nm, sizeof(nm));
            if (nm[4] == '\0' && nm[0] != '\\' && nm[0] != '^')
              local_set(e, nm, ret);
          }
        }
        break;
      }
      default:
        // Desconocido: no podemos saber la longitud. Abortar.
        *pos = code_len;
        break;
      }
    }
    break;
  }

  case AML_PACKAGE_OP: {
    size_t pkg_start = *pos;
    uint32_t pkglen = decode_pkglength(code, code_len, pos);
    size_t pkg_end = pkg_start + pkglen;
    uint32_t nelem = 0;
    if (*pos < pkg_end)
      nelem = code[(*pos)++];
    aml_object_t *pkg = make_package(nelem);
    if (pkg) {
      for (uint32_t i = 0; i < nelem && *pos < pkg_end; i++) {
        pkg->package.elems[i] = eval_term(e, code, code_len, pos);
        if (!pkg->package.elems[i])
          pkg->package.elems[i] = make_integer(0);
      }
    }
    *pos = pkg_end;
    ret = pkg;
    break;
  }

  case AML_VAR_PACKAGE_OP: {
    size_t pkg_start = *pos;
    uint32_t pkglen = decode_pkglength(code, code_len, pos);
    size_t pkg_end = pkg_start + pkglen;
    aml_object_t *nv = eval_term(e, code, code_len, pos);
    uint32_t nelem = nv ? (uint32_t)aml_to_integer(nv) : 0;
    aml_object_t *pkg = make_package(nelem);
    if (pkg) {
      for (uint32_t i = 0; i < nelem && *pos < pkg_end; i++) {
        pkg->package.elems[i] = eval_term(e, code, code_len, pos);
        if (!pkg->package.elems[i])
          pkg->package.elems[i] = make_integer(0);
      }
    }
    *pos = pkg_end;
    ret = pkg;
    break;
  }

  case AML_BUFFER_OP: {
    size_t pkg_start = *pos;
    uint32_t pkglen = decode_pkglength(code, code_len, pos);
    size_t pkg_end = pkg_start + pkglen;
    aml_object_t *sz = eval_term(e, code, code_len, pos);
    uint64_t n = sz ? aml_to_integer(sz) : 0;
    size_t avail = (*pos < pkg_end) ? pkg_end - *pos : 0;
    if (n > avail)
      n = avail;
    ret = make_buffer(code + *pos, (uint32_t)n);
    *pos = pkg_end;
    break;
  }

  case AML_NAME_OP: {
    // Name dentro de método: definir local.
    char nm[256];
    if (read_namestring(code, code_len, pos, nm, sizeof(nm)) < 0)
      break;
    aml_object_t *v = eval_term(e, code, code_len, pos);
    if (nm[4] == '\0' && nm[0] != '\\' && nm[0] != '^') {
      local_set(e, nm, v);
    } else {
      char resolved[512];
      resolve_name(e->scope, nm, resolved, sizeof(resolved));
      aml_node_t *n = lookup_path_abs(resolved);
      if (n)
        n->value = v;
    }
    ret = v;
    break;
  }

  default: {
    // ¿Referencia a un NameString?
    if ((op >= 'A' && op <= 'Z') || op == '_' || op == '\\' || op == '^') {
      (*pos)--; // volver a incluir el primer char
      char nm[256];
      if (read_namestring(code, code_len, pos, nm, sizeof(nm)) < 0)
        break;

      // Caso especial: _OSI
      if (memcmp(nm, "OSI_", 4) == 0 && nm[4] == '\0') {
        // Consumir el arg (string) si lo hay.
        // En contexto de un call, el arg viene a continuación.
        // Lo consumimos si parece un string.
        if (*pos < code_len && code[*pos] == AML_STRING_PREFIX) {
          size_t save = *pos;
          eval_term(e, code, code_len, pos);
          // _OSI devuelve 1 siempre para cualquier string — el firmware
          // cree que somos Windows y usa el camino "óptimo" del DSDT.
          ret = make_integer(1);
          (void)save;
        } else {
          ret = make_integer(1);
        }
        break;
      }

      // _REV: OS ACPI revision. Devolvemos 2 (ACPI 2.0).
      if (memcmp(nm, "REV_", 4) == 0 && nm[4] == '\0') {
        ret = make_integer(2);
        break;
      }

      aml_object_t *v = resolve_and_read(e, nm);
      if (v && v->type == AML_METHOD) {
        // Es un método. Si va seguido de argumentos, los consumimos.
        // Contamos según arg_count del método.
        int nargs = v->method.arg_count;
        aml_object_t *argbuf[AML_MAX_ARGS] = {0};
        for (int i = 0; i < nargs && *pos < code_len; i++) {
          argbuf[i] = eval_term(e, code, code_len, pos);
        }
        ret = eval_method(v, argbuf, nargs, e->depth + 1);
      } else if (v) {
        ret = v;
      } else {
        ret = make_integer(0);
      }
      break;
    }
    // Desconocido: no podemos saber la longitud. Abortamos.
    *pos = code_len;
    break;
  }
  }

done:
  e->depth--;
  return ret;
}

static aml_object_t *eval_method(aml_object_t *m, aml_object_t **args,
                                 int nargs, int depth) {
  if (!m || m->type != AML_METHOD)
    return m;
  if (m->method.code_len == 0)
    return make_integer(0);

  eval_ctx_t e;
  memset(&e, 0, sizeof(e));
  e.scope = m->method.scope;
  e.depth = depth;
  for (int i = 0; i < nargs && i < AML_MAX_ARGS; i++)
    e.args[i] = args[i];

  size_t pos = 0;
  while (pos < m->method.code_len) {
    aml_object_t *r = eval_term(&e, m->method.code, m->method.code_len, &pos);
    (void)r;
    if (e.return_set)
      break;
  }

  // Liberar locales del método.
  local_var_t *lv = e.method_locals;
  while (lv) {
    local_var_t *next = lv->next;
    kfree(lv);
    lv = next;
  }

  return e.return_set && e.ret_val ? e.ret_val : make_integer(0);
}

// ===========================================================================
// API pública
// ===========================================================================
static aml_node_t *lookup_path_caller(const char *path) {
  // Reconoce path con o sin '\' inicial.
  return lookup_path_abs(path);
}

aml_object_t *aml_eval(const char *path) {
  if (!g_ready || !g_root || !path)
    return NULL;
  aml_node_t *n = lookup_path_caller(path);
  if (!n || !n->value)
    return NULL;
  if (n->value->type == AML_METHOD)
    return eval_method(n->value, NULL, 0, 0);
  return n->value;
}

aml_object_t *aml_eval_int_args(const char *path, const uint64_t *args,
                                int nargs) {
  if (!g_ready || !g_root || !path)
    return NULL;
  aml_node_t *n = lookup_path_caller(path);
  if (!n || !n->value)
    return NULL;
  if (n->value->type != AML_METHOD)
    return n->value;
  aml_object_t *objs[AML_MAX_ARGS];
  if (nargs > AML_MAX_ARGS)
    nargs = AML_MAX_ARGS;
  for (int i = 0; i < nargs; i++)
    objs[i] = make_integer(args[i]);
  return eval_method(n->value, objs, nargs, 0);
}

int aml_method_exists(const char *path) {
  if (!g_ready || !g_root || !path)
    return 0;
  aml_node_t *n = lookup_path_caller(path);
  return n && n->value && n->value->type == AML_METHOD;
}

uint64_t aml_to_integer(aml_object_t *obj) {
  if (!obj)
    return 0;
  switch (obj->type) {
  case AML_INTEGER:
    return obj->integer;
  case AML_STRING: {
    const char *s = obj->string.data;
    if (!s)
      return 0;
    while (*s == ' ' || *s == '\t')
      s++;
    int neg = 0;
    if (*s == '-') {
      neg = 1;
      s++;
    } else if (*s == '+')
      s++;
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      s += 2;
      while (1) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9')
          d = c - '0';
        else if (c >= 'a' && c <= 'f')
          d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          d = c - 'A' + 10;
        else
          break;
        v = (v << 4) | d;
        s++;
      }
    } else {
      while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s - '0');
        s++;
      }
    }
    return neg ? (uint64_t)(-(int64_t)v) : v;
  }
  case AML_BUFFER:
    if (obj->buffer.len >= 1)
      return obj->buffer.data[0];
    return 0;
  default:
    return 0;
  }
}

const char *aml_to_string(aml_object_t *obj) {
  if (!obj || obj->type != AML_STRING)
    return "";
  return obj->string.data;
}

uint8_t *aml_to_buffer(aml_object_t *obj, uint32_t *out_len) {
  if (out_len)
    *out_len = 0;
  if (!obj || obj->type != AML_BUFFER)
    return NULL;
  if (out_len)
    *out_len = obj->buffer.len;
  return obj->buffer.data;
}

int aml_package_count(aml_object_t *pkg) {
  if (!pkg || pkg->type != AML_PACKAGE)
    return 0;
  return (int)pkg->package.count;
}

int aml_get_integer(const char *path, uint64_t *out) {
  aml_object_t *o = aml_eval(path);
  if (!o)
    return -1;
  *out = aml_to_integer(o);
  return 0;
}

int aml_get_package(const char *path, aml_object_t ***elems, uint32_t *count) {
  aml_object_t *o = aml_eval(path);
  if (!o || o->type != AML_PACKAGE)
    return -1;
  *elems = o->package.elems;
  *count = o->package.count;
  return 0;
}

// Busca _S5_/_S4_/_S3_ y devuelve los SLP_TYPa/b.
int aml_find_sleep_state(const char *name, uint8_t *slp_typa,
                         uint8_t *slp_typb) {
  aml_object_t *s = aml_eval(name);
  if (!s || s->type != AML_PACKAGE || s->package.count < 2)
    return -1;
  *slp_typa = (uint8_t)aml_to_integer(s->package.elems[0]);
  *slp_typb = (uint8_t)aml_to_integer(s->package.elems[1]);
  return 0;
}

// ===========================================================================
// Init
// ===========================================================================
extern const uint8_t *acpi_get_dsdt(void);
extern int acpi_get_ssdt_count(void);
extern const uint8_t *acpi_get_ssdt(int i);

int aml_init(void) {
  g_root = alloc_node();
  if (!g_root)
    return -1;
  memcpy(g_root->name, "\\___", 5);
  g_root->parent = NULL;

  const uint8_t *dsdt = acpi_get_dsdt();
  if (!dsdt) {
    LOG_ERR("[AML] DSDT no disponible");
    return -1;
  }

  uint32_t dsdt_len = *(const uint32_t *)(dsdt + 4);
  if (dsdt_len < 36) {
    LOG_ERR("[AML] DSDT demasiado corto (%u)", dsdt_len);
    return -1;
  }

  parse_ctx_t c = {
      .data = dsdt + 36,
      .len = dsdt_len - 36,
      .pos = 0,
      .scope = g_root,
  };
  parse_term_list(&c, c.len);

  int ssdt_count = acpi_get_ssdt_count();
  int ssdt_ok = 0;
  for (int i = 0; i < ssdt_count; i++) {
    LOG_INFO("[AML] Procesando SSDT[%d]...", i);
    const uint8_t *ssdt = acpi_get_ssdt(i);
    if (!ssdt) {
      LOG_WARN("[AML] SSDT[%d] = NULL", i);
      continue;
    }
    uint32_t ssdt_len = *(const uint32_t *)(ssdt + 4);
    LOG_INFO("[AML] SSDT[%d]: sig='%c%c%c%c' len=%u", i, ssdt[0], ssdt[1],
             ssdt[2], ssdt[3], ssdt_len);
    if (ssdt_len < 36) {
      LOG_WARN("[AML] SSDT[%d] len<36", i);
      continue;
    }
    parse_ctx_t sc = {
        .data = ssdt + 36,
        .len = ssdt_len - 36,
        .pos = 0,
        .scope = g_root,
    };
    parse_term_list(&sc, sc.len);
    ssdt_ok++;
    LOG_INFO("[AML] SSDT[%d] OK", i);
  }

  g_ready = 1;
  LOG_INFO("[AML] Namespace construido (%d nodos, %d/%d SSDTs)",
           g_node_pool_used, ssdt_ok, ssdt_count);
  return 0;
}

void aml_dump_namespace(void) {
  if (!g_root)
    return;
  LOG_INFO("[AML] Namespace raíz con %d nodos", g_node_pool_used);
}