# Plan_Red — drivers de red de Linux sobre LinuxKPI

Estado: plan (2026-11-05). Rama `claude/project-thread-ll4256`. Sin fusionar a main.

## 1. Problema

El único driver NIC de Aurora es `kernel/net/e1000e.c` + `drivers/aur/e1000e` (nativo, solo 82574L/82540EM/82545EM: los modelos de **QEMU**). En el hardware real no hay red: el Portégé R30-A no tiene NIC funcional (sin DNS ni descargas, ni siquiera netconsole) y la torre i7-12700KF probablemente trae I225/I226 (`igc`).

La pila (skb, ARP, IP, ICMP, UDP, TCP, DHCP, sockets) ya existe y funciona; solo falta hablar con más NICs. Punto de enganche: `netif_t` (`kernel/net/netif.h`: `ops->transmit/open/close`, `netif_rx(skb)`).

## 2. NICs objetivo (hay que confirmar con hardware real)

| Máquina | NIC probable | Driver Linux | Confianza |
|---|---|---|---|
| Portégé R30-A (Haswell, PCH 8-series) | Intel I217-LM/V (PCH integrada) **o** Realtek RTL8168/8111 | `e1000e` (`ich8lan.c`) / `r8169` | Baja: **sin confirmar**. Hay que leer el PCI ID real. |
| Torre i7-12700KF | Intel I225-V/I226-V (o I219-V según placa) | `igc` / `e1000e` (I219) | Media |
| QEMU | e1000e 82574L (ya funciona), virtio-net, rtl8139 | nativo / `virtio_net` | Alta |

Paso 0 inmediato: que el arranque **liste en el log todos los dispositivos PCI de clase 0x02** (vendor:device) y los guarde en `aurora.log`. Con una sola línea de ese log en cada máquina se decide el orden de los drivers.

## 3. Estrategia

Dos vías para cada NIC; se elige por coste:

- **Port del driver de Linux** sobre LinuxKPI (`.kur`): hereda los errata de cada chip (I217/I219 y `igc` tienen mucho: reset de PHY, ULP, `ME` lockout, EEE). Es lo pedido. Requiere una capa nueva `net` en LinuxKPI.
- **Driver nativo corto** (`.aur`), solo si el chip es simple (RTL8168 sin firmware, virtio-net). Fallback si el port se encalla.

### Capa nueva LinuxKPI-NET (L-NET)

Lo que falta en `include/linuxkpi/linux/` y `kernel/linuxkpi/`:

1. `struct net_device` + `alloc_etherdev`/`register_netdev`/`unregister_netdev`/`netdev_priv`/`netif_carrier_*`/`netif_*_queue`, con un puente a `netif_t` de Aurora (open/close/transmit/MAC/estadísticas).
2. `struct sk_buff` de Linux (`alloc_skb`, `netdev_alloc_skb_ip_align`, `skb_put/reserve/push/pull`, `dev_kfree_skb*`, `napi_gro_receive`, `eth_type_trans`, frags básicos) con conversión a `skb_t` de Aurora al entregar en RX y desde `skb_t` en TX (copia, sin GRO/TSO al inicio: `NETIF_F_SG`/TSO/`csum` apagados salvo checksum RX).
3. NAPI (`netif_napi_add`, `napi_schedule`, `napi_complete_done`) sobre un kthread/softirq de la capa (ya hay `softirq.c`, `workqueue.c`, `irq.c`).
4. `ethtool` mínimo (solo `get_drvinfo/link_ksettings` para registrar sin romper), `mii`/`phylib` solo lo que pidan los drivers (e1000e e igc llevan su propio PHY; r8169 usa `phylib` → shim mínimo o parche).
5. DMA coherente y mapeo streaming ya existen (L3/L5: `dma_alloc_coherent`, `dma_map_single`); MSI/MSI-X ya hecho para i915; hace falta probar MSI-X múltiple (igc).
6. `eeprom`/NVM, `firmware` (r8169 usa `rtl_nic/*.fw`; ya existe `firmware.c`), `timer`/`delayed_work` (existen).

Descarga de fuentes: `scripts/fetch-linux-drivers.sh` extrae solo `drivers/gpu`, `virtio`, `dma-buf`, `video`; hay que añadir `drivers/net/ethernet/{intel,realtek}`, `drivers/net/mii.c`, `drivers/net/phy` (si hace falta), `net/core` NO (se usa el nuestro).

## 4. Fases

| Fase | Contenido | Criterio de salida |
|---|---|---|
| **N0** | Log de NIC PCI al arrancar (todas las clases 0x02); documento con el ID real de cada máquina | línea `[NET] pci 02:00 8086:xxxx` en `aurora.log` |
| **N1** | L-NET: `net_device`, `sk_buff`, NAPI, puente a `netif_t`; módulo de prueba `lkpi_nettest` (NIC falsa, ping por loopback) | prueba del kernel nueva en verde, 211+/211+ |
| **N2** | **`virtio_net`** de Linux (el más simple, probado en QEMU con `-nic model=virtio-net-pci`) como primer driver real de la capa | DHCP + ping 10.0.2.2 en QEMU |
| **N3** | **`e1000e` de Linux** (82574L en QEMU para validar; luego ich8lan: I217/I218/I219) | DHCP en QEMU con el driver Linux; I217/I219 lo valida Alvaro en hardware |
| **N4** | **`r8169`** (RTL8168/8111/8125) | Alvaro lo valida si el Portégé es Realtek; en QEMU `-nic model=rtl8139` NO sirve (otro chip): se prueba con VFIO/hardware |
| **N5** | **`igc`** (I225/I226) | validación en torre |
| **N6** | Autocarga: `lkpi_boot.c` elige el driver por PCI ID (como con Intel i915); `net.auto=off` lo desactiva; el driver nativo e1000e se cede si el de Linux toma la NIC | arranque sin parámetros |

Orden condicionado por N0: si el Portégé es Realtek, N4 sube antes que N3.

## 5. Riesgos

- **Sin hardware en el entorno de desarrollo**: solo se verifica en QEMU (e1000e 82574L, virtio-net). Los chips PCH (I217/I219), `igc` y Realtek solo los puede validar Alvaro; cada fase entrega un registro (`aurora.log`) para depurar sin puerto serie.
- I217/I219: el PHY puede estar bloqueado por el ME o en ULP; el driver de Linux lo gestiona, un driver propio no.
- `igc` y `r8169` usan más API (ptp, phylib, `page_pool`, XDP) que el shim no tiene; se desactivan por `#if`/parche (`kernel/linuxkpi/patches/`).
- Coexistencia con `e1000e.aur`: no deben tomar la misma NIC (`aur.skip=8086:xxxx`).

## 6. Fuera de alcance

Wi-Fi, offloads (TSO/GRO), VLAN, XDP, SR-IOV, L8/L9 (esperan a Alvaro).

## 7. Estado (2026-11-05)

| Fase | Estado |
|---|---|
| N0 log de NIC PCI | hecho |
| N1 capa LinuxKPI-NET (`net_device`, `sk_buff`, NAPI en tasklet, mini-phylib/MDIO, puente `netif_t`) | hecho (`kernel/linuxkpi/net/`, `kernel/net/netif_lkpi.c`) |
| N3 `e1000e` de Linux 6.18 (`lkpi_e1000e.kur`) | **verificado en QEMU** (82574L, MSI-X): enlace 1000 Mbps, DHCP 10.0.2.15. Pendiente: camino MSI (el test MSI del driver falla en QEMU; los I217/I219 reales usan MSI) e INTx |
| N4 `r8169` de Linux 6.18 (`lkpi_r8169.kur`) | compila y enlaza; **sin probar** (QEMU no emula RTL8168) |
| N2 `virtio_net`, N5 `igc`, N6 autocarga | pendientes |

Prueba: `scripts/qemu-drive.py --cmdline "aur.skip=8086:10d3 lkpi.load=linuxkpi,lkpi_e1000e" --extra=-nic --extra=user,model=e1000e --done "DHCP] lease"`.
