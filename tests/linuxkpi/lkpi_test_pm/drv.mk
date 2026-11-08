# Pruebas de LinuxKPI de L9.2/L9.3: runtime PM (get/put, autosuspend,
# forbid/allow, D3hot de PCI) y suspend/resume de dispositivos. El motor de
# runtime PM solo es real con `lkpi.runtime_pm=on` en la linea del kernel;
# sin eso se prueban las garantias del modo "siempre activo".
LKPI_DRV_SRCDIR := tests/linuxkpi/lkpi_test_pm
LKPI_DRV_SRCS   := test_pm.c
LKPI_DRV_CFLAGS := -Itests/linuxkpi
LKPI_DRV_INSTALL := linuxkpi/tests
