# Roadmap de implementación de discos para Aurora OS

Estado actual y plan completo revisado tras completar ATA DMA.

---

## Estado actual

### ✅ Fase 0: Block layer + ATA PIO — COMPLETADA

| Componente | Estado |
|---|---|
| block.h / block.c (bio, block_device, block_ops) | ✅ |
| ata_common.h / ata_common.c (helpers compartidos) | ✅ |
| ata_pio.h / ata_pio.c (detección + lectura/escritura PIO) | ✅ |
| LBA28 + LBA48 (selección automática) | ✅ |
| Detección de capacidades (PIO modes, DMA, UDMA, NCQ, sector size) | ✅ |
| Reintentos con reset de canal | ✅ |
| Fallback LBA48 → LBA28 si ABRT | ✅ |
| Compatibilidad de par master/slave (ATA+ATA, ATA+ATAPI, canal solo) | ✅ |
| Detección en 3 fases (probe ATA, probe ATAPI, finalize) | ✅ |

Tests: 7+ (hda, capacidad, read sector 0, read multi, write/read, oob read, oob write).

---

### ✅ Fase 1: ATAPI — COMPLETADA

| Componente | Estado |
|---|---|
| atapi.h / atapi.c | ✅ |
| Detección por firma 0x14/0xEB con SRST condicional | ✅ |
| Comandos SCSI: TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY (10), READ (10) | ✅ |
| Detección de medio y media change | ✅ |
| Nombres globales srN (como Linux) | ✅ |
| Chunking de READ(10) a ≤31 bloques (límite 0xFFFE bytes) | ✅ |

Tests: 7 (sr0, read-only, sector size, medio, PVD, write fail, oob read).

---

### ✅ Fase 2: ATA DMA — COMPLETADA

| Componente | Estado |
|---|---|
| ata_dma.h / ata_dma.c | ✅ |
| Detección del BMIDE vía PCI (BAR4) | ✅ |
| PRDT multi-entrada (paginada, respeta límite 64 KB) | ✅ |
| Completion con timeout real (5 s) | ✅ |
| IRQ handler para IRQ 14/15 | ✅ |
| Sincronización del IRQ handler con la wait queue (race SMP) | ✅ |
| Fallback automático a PIO si DMA falla | ✅ |
| pci_enable_bus_mastering limpia INT_DISABLE | ✅ |
| nIEN = 0 (IRQs del disco habilitadas) | ✅ |
| FLUSH CACHE tras escritura DMA (fix VirtualBox) | ✅ |
| Política de fallos de DMA (3 strikes → PIO) | ✅ |
| Soft reset antes de fallback a PIO | ✅ |

Tests: 3 (BMIDE listo, leer 4 KB, escribir y leer 4 KB).

**Causa raíz del bug original (ya resuelto):**

El problema **no era** compatibility mode vs native mode, ni que el PIIX3 no soportase DMA. Era una **condición de carrera SMP** entre el hilo que iniciaba la transferencia y el IRQ handler:

1. El waiter comprobaba `irq_pending == 0`, se disponía a encolarse.
2. La IRQ llegaba en otro núcleo, ponía `irq_pending = 1`, y llamaba a `wake_up_all()`.
3. La cola estaba vacía: no despertaba a nadie.
4. El waiter se encolaba y dormía para siempre → timeout a los 5 s.

**Solución aplicada:** el IRQ handler ahora modifica `irq_pending`/`irq_error`/`irq_count` y llama a `wake_up_all_locked()` **bajo el mismo lock que usa `wait_common`** (`dma->irq_wq.lock`). Con esto:
- Si la IRQ llega antes de que el waiter compruebe la condición → el waiter ve `irq_pending=1` y no duerme.
- Si la IRQ llega después de que el waiter se encole → `wake_up_all_locked` lo encuentra y lo despierta.
- Las IRQs solo se deshabilitan durante los microsegundos de las secciones críticas; la espera ocurre con IRQs habilitadas.

**Lección arquitectónica:** cualquier estado de completación que sea modificado por un IRQ handler y consumido por una wait queue debe protegerse con **el lock de la wait queue**, no con un lock separado.

---

### ✅ PCI — MEJORADO

| Componente | Estado |
|---|---|
| Enumeración completa (bus, slot, func) | ✅ |
| Lectura de BARs (I/O y MMIO, 32/64-bit) | ✅ |
| Detección de bridges PCI-to-PCI (bus secundario) | ✅ |
| pci_find_device, pci_get_device, pci_enable_bus_mastering | ✅ |
| pci_read_bar wrapper | ✅ |

---

## Roadmap completo actualizado

### Fase 0: Block layer + ATA PIO ✅ COMPLETADA
(Ver resumen arriba.)

### Fase 1: ATAPI ✅ COMPLETADA
(Ver resumen arriba.)

### Fase 2: ATA DMA ✅ COMPLETADA
(Ver resumen arriba.)

**Deuda técnica pendiente en Fase 2:**

| Item | Prioridad | Notas |
|---|---|---|
| Check de `BM_STATUS_ACTIVE` tras el IRQ | Media | Verificar que el bus master haya terminado, no solo que el IRQ llegó |
| Mensaje "sin peer" en `ata_identify` para slots vacíos | Baja | Distinguir `0x00` (slot vacío) de `0xFF` (bus flotante) |
| LBA48 nunca ejecutado en pruebas | Alta | El VDI de 64 GB cabe en LBA28; hace falta un VDI >128 GB |
| Orden de asignación hda/hdb | Media | Actualmente invertido respecto a Linux (hda = slave, hdb = master) |

---

### 🔴 Fase 3: AHCI (SATA) — PENDIENTE

**Objetivo:** soportar SATA nativo en QEMU/VirtualBox con `-machine q35` o con controlador AHCI.

| Componente | Estado |
|---|---|
| ahci.h / ahci.c | ⏳ |
| Detección del controlador AHCI vía PCI (clase 0x01, subclase 0x06) | ⏳ |
| Mapeo de BAR5 (ABAR) con MMIO_MAP_BASE | ⏳ |
| Inicialización del HBA (GHC, CAP, PI, VS) | ⏳ |
| Puertos implementados (PI) | ⏳ |
| Command List y Command Table | ⏳ |
| FIS (H2D, D2H, PIO Setup, DMA Setup) | ⏳ |
| PRDT por comando (múltiples entradas) | ⏳ |
| IRQs por puerto | ⏳ |
| ATAPI sobre AHCI (CD/DVD) | ⏳ |
| Hot-plug (SSTS detection + debounce) | ⏳ |
| Port multiplier (opcional) | ⏳ |

**Diferencias clave respecto a ATA DMA:**

- El completion model es distinto: no hay IRQ por transferencia, sino un bit por puerto en `IS` (Interrupt Status) del HBA.
- Cada comando tiene su propia slot en la Command List; el driver debe gestionar slots libres.
- La wait queue debe ser **por slot de comando**, no por canal.
- Los FIS son estructuras alineadas en memoria física, no registros I/O.

**Tests:**

- ahci: controlador detectado (ABAR, CAP, PI).
- ahci: puerto con disco (SSTS = 3).
- ahci: identificar disco (IDENTIFY DEVICE vía FIS).
- ahci: leer sector 0.
- ahci: escribir sector.
- ahci: ATAPI (INQUIRY, READ CAPACITY, READ 10).
- ahci: hot-plug (insertar y quitar).
- ahci: NCQ con 2+ comandos en vuelo.

**Tiempo estimado:** 1-2 semanas.

**Prioridad:** media-alta. Es el siguiente paso natural tras ATA DMA; sin AHCI, los discos SATA modernos solo se ven en modo legacy IDE.

---

### 🟡 Fase 4: Bridges y quirks — PENDIENTE

**Objetivo:** compatibilidad con hardware exótico.

| Componente | Estado |
|---|---|
| pci_quirks.h / pci_quirks.c | ⏳ |
| Tabla de quirks por vendor:device | ⏳ |
| Detección de bridges PCI-to-PCI | ⏳ |
| Detección de port multiplier en AHCI | ⏳ |
| Detección de discos 4Kn (sector 4096) | ⏳ |
| Detección automática de modo IDE legacy | ⏳ |

**Quirks conocidos:**

| Vendor:Device | Nombre | Quirk |
|---|---|---|
| 8086:7010 | PIIX3 IDE | Compatibility mode por defecto; funcional en legacy |
| 8086:7111 | PIIX4 IDE | OK |
| 8086:2922 | ICH9 AHCI | AHCI |
| 10DE:056C | NVIDIA MCP | AHCI o IDE |
| 1002:4390 | AMD SB700 | AHCI o IDE |
| 1B4B:9128 | Marvell 88SE9128 | AHCI + port multiplier |
| 197B:2368 | JMicron JMB368 | Quirk de reset |

**Nota:** el quirk del PIIX3 en compatibility mode **no requiere cambiarlo a native mode** — el driver funciona correctamente con los puertos I/O fijos (0x1F0/0x170). Los bridges PCI-to-PCI solo son relevantes si aparece hardware detrás de un bridge.

**Tiempo estimado:** 1 semana.
**Prioridad:** baja.

---

### 🔴 Fase 5: Particiones — PENDIENTE

**Objetivo:** exponer particiones como block devices independientes.

| Componente | Estado |
|---|---|
| partition.h / partition.c | ⏳ |
| MBR (4 primarias + extendidas) | ⏳ |
| GPT (hasta 128 particiones) | ⏳ |
| Detección de FS por firma | ⏳ |

**Integración:**

- Tras registrar un disco, parsear su tabla de particiones.
- Crear un `block_device_t` por partición con `is_partition = 1`, `parent`, `start_lba`.
- Nombres: `hda1`, `hda2`, `sda1`, `sr0` (los CDs no tienen particiones).
- El bloque de arranque (LBA 0) no se expone como partición.

**Tests:**

- partition: hda tiene una partición EFI.
- partition: hda1 empieza en LBA X.
- partition: leer sector 0 de hda1.
- partition: disco sin tabla → no crea particiones.

**Tiempo estimado:** 3-5 días.
**Prioridad:** alta (bloquea Fase 6).

---

### 🔴 Fase 6: Filesystems — PENDIENTE

**Objetivo:** montar FS y leer/escribir archivos.

| Componente | Estado |
|---|---|
| fat32.h / fat32.c / fat32_vfs.c | ⏳ |
| ext2.h / ext2.c / ext2_vfs.c | ⏳ |
| iso9660.h / iso9660.c | ⏳ |

**FAT32 (prioridad 1):**

- BPB, FAT, cluster chains.
- LFN (Long File Name).
- Lectura y escritura de archivos.
- Creación y borrado de directorios.
- Integración con VFS.

**ext2 (prioridad 2):**

- Superblock, block groups, inodes.
- Directorios, archivos, symlinks.
- Permisos.

**ISO9660 (prioridad 3):**

- PVD, directorios, archivos.
- Soporte Joliet/Rock Ridge (opcional).

**Tests:**

- fat32: montar hda1.
- fat32: leer archivo.
- fat32: escribir archivo.
- fat32: crear directorio.
- fat32: persistencia tras reboot.
- ext2: montar, leer, escribir, symlink.
- iso9660: montar sr0, leer archivo.

**Tiempo estimado:**

- FAT32: 1-2 semanas.
- ext2: 2-3 semanas.
- ISO9660: 3-5 días.

**Prioridad:** muy alta.

---

### 🟡 Fase 7: Optimizaciones — PENDIENTE

**Objetivo:** rendimiento y robustez.

| Componente | Estado |
|---|---|
| Block cache (LRU) | ⏳ |
| Write-back con flush periódico | ⏳ |
| NCQ en AHCI | ⏳ |
| Scatter-gather en DMA/AHCI | ⏳ |
| Async I/O (bio asíncrono real con completion) | ⏳ |
| Prefetch de sectores | ⏳ |

**Tiempo estimado:** 1-2 semanas.
**Prioridad:** baja.

---

### 🟡 Fase 8: Instalador — PENDIENTE

**Objetivo:** Aurora OS instalable en un disco.

| Componente | Estado |
|---|---|
| apps/installer/main.c | ⏳ |
| Detección de discos | ⏳ |
| Formateo de partición EFI | ⏳ |
| Copia de bootloader, kernel e initramfs | ⏳ |
| Configuración de entrada de arranque UEFI | ⏳ |

**Tiempo estimado:** 1 semana.
**Prioridad:** media.

---

## Timeline estimado

| Fase | Descripción | Tiempo | Prioridad |
|---|---|---|---|
| 0 | Block layer + ATA PIO | ✅ | — |
| 1 | ATAPI | ✅ | — |
| 2 | ATA DMA | ✅ | — |
| 3 | AHCI (SATA) | 1-2 semanas | Media-alta |
| 4 | Bridges y quirks | 1 semana | Baja |
| 5 | Particiones | 3-5 días | Alta |
| 6 | Filesystems (FAT32) | 1-2 semanas | Muy alta |
| 7 | Optimizaciones | 1-2 semanas | Baja |
| 8 | Instalador | 1 semana | Media |

**Total restante:** 6-9 semanas de trabajo a tiempo parcial.

---

## Lecciones aprendidas (para futuras fases)

### Sobre concurrencia y IRQs

1. **El estado de completación debe protegerse con el lock de la wait queue**, no con un lock separado. Cualquier campo que un IRQ handler modifique y que una `wait_event_*` consuma debe estar bajo el mismo lock.
2. **Las IRQs deben permanecer habilitadas durante la espera.** Solo se deshabilitan dentro de las secciones críticas (comprobar condición + encolarse).
3. **Orden de locks documentado y respetado.** En ATA DMA: `dma->lock` → `dma->irq_wq.lock`. Nunca al revés.
4. **El IRQ handler nunca debe usar la variante `_locked`-menos de las funciones de wake.** Si ya tiene el lock cogido, debe llamar a `wake_up_all_locked`.
5. **Sondear (polling) es aceptable cuando `preempt_count > 0`**, pero nunca debe ser el camino principal.

### Sobre compatibilidad de pares master/slave

6. **Un ATAPI contamina al disco ATA del mismo canal.** Forzar PIO en el disco cuando el peer es ATAPI. Confirmado por la práctica.
7. **Un canal con un solo dispositivo SÍ soporta DMA.** No es necesario forzar PIO por miedo. Esta regla restrictiva estaba basada en una hipótesis incorrecta.
8. **Distinguir "peer es ATAPI" de "no hay peer".** Son casos distintos y deben generar decisiones distintas.

### Sobre VirtualBox vs QEMU

9. **VirtualBox emula el PIIX3 de forma más realista que QEMU.** Los bugs de timing (IRQ antes de que el waiter esté listo, IRQ antes de que el disco haya flusheado a medio) solo aparecen en VirtualBox.
10. **FLUSH CACHE tras escritura DMA es obligatorio** para garantizar persistencia antes de dar la operación por completada.

### Sobre tests

11. **Los tests de driver no deben asumir el contenido del disco.** Un VDI virgen no tiene MBR. El test debe verificar que la operación de lectura tuvo éxito, y solo si hay firma, comprobar que sea 0x55AA.
12. **Un test de DMA no debe forzar DMA si la política del driver dice PIO.** El test debe usar `bdev_read`/`bdev_write` a través del block layer, no llamar a `ata_dma_*` directamente.