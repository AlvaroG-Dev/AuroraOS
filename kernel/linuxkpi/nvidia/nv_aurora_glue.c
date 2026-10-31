// Funciones de kernel-open/nvidia que Aurora no compila (NVLink, NVSwitch, UVM,
// IMEX, plataforma Tegra) con el valor que el driver espera cuando no existen.
// scripts/nvidia-link.sh lo enlaza antes de generar stubs automaticos, que
// devolverian 0 aunque 0 signifique "hecho" (p. ej. nv_uvm_event_interrupt).

#define NV_OK                    0
#define NV_ERR_NO_INTR_PENDING   0x00000050

int nvlink_core_init(void) { return 0; }
int nvswitch_init(void) { return 0; }
int nv_caps_imex_init(void) { return 0; }
int nv_uvm_init(void) { return 0; }
int nv_platform_count_devices(void) { return 0; }
unsigned int nv_clk_get_handles(void *nv) { (void)nv; return NV_OK; }

// NV_OK significaria "la interrupcion era de UVM" y nv_isr la daria por
// atendida; sin UVM nunca lo es.
unsigned int nv_uvm_event_interrupt(const unsigned char *uuid)
{
    (void)uuid;
    return NV_ERR_NO_INTR_PENDING;
}
