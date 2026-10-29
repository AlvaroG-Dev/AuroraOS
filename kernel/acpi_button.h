// kernel/acpi_button.h
#ifndef KERNEL_ACPI_BUTTON_H
#define KERNEL_ACPI_BUTTON_H

// Arranca el sondeo del boton de encendido fijo. Llamar tras acpi_pm_init().
// 0 si queda activo.
int acpi_button_init(void);

#endif
