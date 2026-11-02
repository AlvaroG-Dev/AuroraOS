// kernel/acpi_pm.h
#ifndef KERNEL_ACPI_PM_H
#define KERNEL_ACPI_PM_H

#include <stdint.h>

// Init: lee _PSS, _PCT, _PPC, _S5_ del namespace. Llamar tras aml_init().
int acpi_pm_init(void);

// Estado actual del subsistema PM.
void acpi_pm_dump(void);

// Activa el modo ACPI (SCI_EN) con ACPI_ENABLE si el firmware lo dejo en
// modo legacy. 0 si ACPI esta activo.
int acpi_pm_enable(void);

// Backends ACPI (vuelven con -1 si no hay o no surten efecto). Los usa
// power.c; el resto del kernel debe llamar a power_request().
int acpi_pm_poweroff(void); // S5
int acpi_pm_restart(void);  // RESET_REG del FADT

// Acciones (envoltorios de power_request)
void acpi_pm_reboot(void);
void acpi_pm_shutdown(void);
void acpi_pm_suspend(void); // S3

// Aplicar el P-state más alto. Devuelve 0 si OK.
int acpi_pm_set_max_performance(void);

#endif