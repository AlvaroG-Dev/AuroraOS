// kernel/acpi_pm.h
#ifndef KERNEL_ACPI_PM_H
#define KERNEL_ACPI_PM_H

#include <stdint.h>

// Init: lee _PSS, _PCT, _PPC, _S5_ del namespace. Llamar tras aml_init().
int acpi_pm_init(void);

// Estado actual del subsistema PM.
void acpi_pm_dump(void);

// Acciones
void acpi_pm_reboot(void);
void acpi_pm_shutdown(void);
void acpi_pm_suspend(void); // S3

// Aplicar el P-state más alto. Devuelve 0 si OK.
int acpi_pm_set_max_performance(void);

#endif