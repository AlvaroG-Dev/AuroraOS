// kernel/iommu.h
//
// IOMMU Intel VT-d (L9). Traduccion de DMA con tablas de segundo nivel por
// dispositivo, para que un driver de GPU/NIC de Linux (LinuxKPI) no pueda
// escribir en cualquier RAM.
//
// Politica (ver Plan_LinuxKPI.md, L9):
//   * `iommu=on` en la linea de comandos lo activa; sin eso (o con
//     `iommu=off`) Aurora no toca el hardware. Por defecto esta apagado
//     mientras no se haya probado en equipo real.
//   * Al activarlo, TODOS los dispositivos PCI quedan en pass-through (los
//     drivers propios del kernel -AHCI, xHCI, ...- no cambian) y solo los
//     dispositivos que usan la API DMA de LinuxKPI reciben un dominio
//     privado con sus tablas (iommu_domain_get).
//   * Las RMRR del firmware se mapean 1:1 en el dominio privado.
//   * `iommu.igfx=off` deja la iGPU Intel en pass-through.
//   * Un dispositivo que supera `iommu.fault_limit` fallos (por omision 10000,
//     0 = sin limite) se pone en cuarentena: se le quita el bus master. Al
//     desvincular su driver se libera el dominio y el contador vuelve a 0.
//   * Los fallos de traduccion llegan por interrupcion (o por sondeo con
//     iommu_fault_poll) y se cuentan por dominio.
#ifndef KERNEL_IOMMU_H
#define KERNEL_IOMMU_H

#include <stdbool.h>
#include <stdint.h>

#define IOMMU_PROT_R 1u // el dispositivo puede LEER la memoria
#define IOMMU_PROT_W 2u // el dispositivo puede ESCRIBIR la memoria
#define IOMMU_ERR (~0ULL)

typedef struct iommu_domain iommu_domain_t;

typedef struct {
  uint64_t maps, unmaps; // llamadas a iommu_dma_map / unmap
  uint64_t pages_live;   // paginas mapeadas ahora mismo
  uint64_t faults;       // fallos de traduccion atribuidos al dominio
  uint64_t last_fault_iova;
  uint8_t last_fault_reason;
  uint8_t quarantined; // el dispositivo superó iommu.fault_limit y perdió bus master
  uint16_t bdf;
  uint16_t did;
} iommu_domain_stats_t;

typedef struct {
  int units;           // unidades DRHD con traduccion activa
  int domains;         // dominios privados
  uint64_t faults;     // total de fallos registrados
  uint64_t unattributed_faults; // sin dominio privado (pass-through)
} iommu_stats_t;

// Detecta DMAR e inicializa las unidades segun la linea de comandos. Devuelve
// 1 si queda traduccion activa. Se puede llamar varias veces (idempotente).
int iommu_init(void);
bool iommu_active(void);

// Dominio privado del dispositivo `bdf` (bus<<8 | slot<<3 | func); lo crea y
// lo engancha la primera vez. NULL si no hay IOMMU para ese dispositivo (sigue
// en pass-through) o no se puede crear.
iommu_domain_t *iommu_domain_get(uint16_t bdf);
// Devuelve el dominio privado si ya existe (sin crearlo).
iommu_domain_t *iommu_domain_find(uint16_t bdf);
// Devuelve el dispositivo a pass-through y libera su dominio. Todo lo
// mapeado se pierde: el driver debe haber hecho unmap antes.
void iommu_domain_release(uint16_t bdf);

// Mapea [phys, phys+size) y devuelve la direccion de bus (IOVA) o IOMMU_ERR.
// La IOVA queda por debajo de `mask` + 1 y conserva el desplazamiento dentro
// de la pagina. Seguro en contexto de interrupcion.
uint64_t iommu_dma_map(iommu_domain_t *d, uint64_t phys, uint64_t size,
                       unsigned prot, uint64_t mask);
void iommu_dma_unmap(iommu_domain_t *d, uint64_t iova, uint64_t size);
// Traduce una IOVA mapeada a direccion fisica (0 si no esta mapeada).
uint64_t iommu_iova_to_phys(iommu_domain_t *d, uint64_t iova);

void iommu_domain_get_stats(const iommu_domain_t *d, iommu_domain_stats_t *out);
void iommu_get_stats(iommu_stats_t *out);

// Lee y limpia los registros de fallos (lo hace tambien la interrupcion).
// Devuelve cuantos fallos nuevos procesó.
int iommu_fault_poll(void);

// Sueño del sistema: apaga la traduccion y la reprograma al volver (las
// tablas viven en RAM). iommu_suspend devuelve 0.
int iommu_suspend(void);
int iommu_resume(void);

#endif
