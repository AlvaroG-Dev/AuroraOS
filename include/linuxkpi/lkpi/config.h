/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Configuracion fija de la capa LinuxKPI de Aurora: los CONFIG_* que ven los
 * drivers de Linux (equivale al autoconf.h generado por Kconfig). Solo se
 * definen las opciones que la capa implementa; el resto queda sin definir
 * (desactivado), como en un .config minimo.
 */
#ifndef _LKPI_CONFIG_H
#define _LKPI_CONFIG_H

#define CONFIG_X86 1
#define CONFIG_X86_64 1
#define CONFIG_64BIT 1
#define CONFIG_MMU 1
#define CONFIG_SMP 1
#define CONFIG_NR_CPUS 64
#define CONFIG_HZ 1000          /* el tick de Aurora es de 1 ms */
#define CONFIG_PAGE_SHIFT 12
#define CONFIG_PHYS_ADDR_T_64BIT 1
#define CONFIG_ARCH_DMA_ADDR_T_64BIT 1
#define CONFIG_GENERIC_ATOMIC64 0
#define CONFIG_PRINTK 1
#define CONFIG_MODULES 1
#define CONFIG_PCI 1
#define CONFIG_PCI_MSI 1
#define CONFIG_PCIEPORTBUS 1
#define CONFIG_SYSFS 1
#define CONFIG_HAS_DMA 1
#define CONFIG_HAS_IOPORT 1
#define CONFIG_HAS_IOPORT_MAP 1
#define CONFIG_GENERIC_MSI_IRQ 1
#define CONFIG_X86_LOCAL_APIC 1
#define CONFIG_X86_IO_APIC 1
#define CONFIG_X86_PAT 1
#define CONFIG_HAS_IOMEM 1
#define CONFIG_ARCH_HAS_FAST_MULTIPLIER 1

#define LKPI_AURORA 1

#endif
