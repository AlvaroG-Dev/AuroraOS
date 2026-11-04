# Red en QEMU: qué esperar

`make run*` usa `-nic user,model=e1000e` (SLIRP). Con SLIRP:

| Prueba | Resultado esperado |
|---|---|
| `ping 10.0.2.2` (gateway) | OK (lo responde SLIRP) |
| `ping 10.0.2.3` (DNS) | OK |
| `ping 8.8.8.8` | **100% de pérdida salvo que el host permita ICMP sin privilegios** |
| TCP/UDP a internet (wget, curl, DNS) | OK |

SLIRP reenvía los ICMP a internet usando sockets ICMP "ping" del host, que solo
existen si el GID de quien lanza QEMU está en `net.ipv4.ping_group_range`.
En Debian/Kali el valor por defecto es `1 0` (desactivado). Comprobar/activar en el host:

    sysctl net.ipv4.ping_group_range
    sudo sysctl -w net.ipv4.ping_group_range="0 2147483647"

No es un fallo de AuroraOS: DHCP, ARP, ICMP y el driver e1000e.aur funcionan
(verificado en QEMU: lease 10.0.2.15, gw 10.0.2.2, ping a 10.0.2.2/10.0.2.3 sin pérdida).
Para probar internet sin tocar sysctl, usar `wget`/`curl` o el modo TAP (`make run-smp-kvm-ahci-tap`).
