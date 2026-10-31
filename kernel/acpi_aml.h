// kernel/acpi_aml.h
#ifndef KERNEL_ACPI_AML_H
#define KERNEL_ACPI_AML_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Intérprete AML mínimo para ACPI.
//
// Soporta lo necesario para:
//   - _PSS (Performance Supported States)
//   - _PCT (Performance Control)
//   - _PPC (Performance Present Capabilities)
//   - _S5_ (Soft Off), _S4_ (Hibernate), _S3_ (Suspend)
//   - _STA, _HID, _UID, _CRS (parcial)
//
// NO soporta (documentado):
//   - OperationRegions no-SystemMemory (SMBus, PCI_Config, ...)
//   - Notify / GPE methods
//   - Some advanced opcodes
// ---------------------------------------------------------------------------

typedef struct aml_object aml_object_t;
typedef struct aml_node aml_node_t;

typedef enum {
  AML_UNINITIALIZED = 0,
  AML_INTEGER,
  AML_STRING,
  AML_BUFFER,
  AML_PACKAGE,
  AML_FIELD,
  AML_DEVICE,
  AML_EVENT,
  AML_METHOD,
  AML_MUTEX,
  AML_OPREGION,
  AML_POWERRES,
  AML_PROCESSOR,
  AML_THERMALZONE,
  AML_BUFFFIELD,
  AML_REFERENCE,
  AML_ALIAS,
} aml_type_t;

struct aml_object {
  aml_type_t type;
  union {
    uint64_t integer;
    struct {
      char *data;
      uint32_t len;
    } string;
    struct {
      uint8_t *data;
      uint32_t len;
    } buffer;
    struct {
      aml_object_t **elems;
      uint32_t count;
    } package;
    struct {
      uint8_t *code;
      uint32_t code_len;
      uint8_t arg_count;
      uint8_t flags;
      aml_node_t *scope;
    } method;
    struct {
      uint64_t base;
      uint32_t len;
      uint8_t *buf;
    } opregion;
    aml_node_t *reference;
  };
};

struct aml_node {
  char name[5]; // 4-char ACPI name + NUL
  aml_object_t *value;
  aml_node_t *parent;
  aml_node_t *children;
  aml_node_t *sibling;
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
int aml_init(void);            // parsea DSDT + SSDTs, construye namespace
void aml_dump_namespace(void); // debug

// ---------------------------------------------------------------------------
// API de evaluación
// ---------------------------------------------------------------------------

// Evalúa un path (con o sin '\' inicial) y devuelve el objeto.
// Si el nodo es un método, lo ejecuta. El resultado es del intérprete,
// no lo liberes.
aml_object_t *aml_eval(const char *path);

// Igual que aml_eval, pero pasa `nargs` argumentos enteros (Arg0..) al
// metodo (p. ej. \_PTS(5)). Si el nodo no es un metodo devuelve su valor.
aml_object_t *aml_eval_int_args(const char *path, const uint64_t *args,
                                int nargs);
// Registra en el log los dispositivos de boton/EC/tapa y los metodos de GPE
// del espacio de nombres (diagnostico del boton de encendido).
void aml_diag_power(void);
// 1 si `path` existe y es un metodo.
int aml_method_exists(const char *path);

// Helpers
uint64_t aml_to_integer(aml_object_t *obj);
const char *aml_to_string(aml_object_t *obj);
uint8_t *aml_to_buffer(aml_object_t *obj, uint32_t *out_len);
int aml_package_count(aml_object_t *pkg);

// Conveniencia
int aml_get_integer(const char *path, uint64_t *out);
int aml_get_package(const char *path, aml_object_t ***elems, uint32_t *count);

// Busca el objeto _S5_ (o _S4_, _S3_) y devuelve SLP_TYPa y SLP_TYPb.
// Devuelve 0 si OK, -1 si no se encuentra.
int aml_find_sleep_state(const char *name, uint8_t *slp_typa,
                         uint8_t *slp_typb);

#endif