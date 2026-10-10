# Plan — Stack TCP/IP propio para Aurora OS

Documento de diseño. Esto no es un sprint, es el proyecto completo. Vamos por partes.

---

## 1. Principios de diseño

Antes de escribir código, fijamos las reglas. Si alguna vez el plan se desvía, volvemos aquí.

1. **Capas estrictas:** Cada capa solo habla con la de arriba y la de abajo. Ethernet no sabe de IP. IP no sabe de TCP. TCP no sabe de sockets. Esto es lo que permite testear cada capa en aislamiento.
2. **Cero asignaciones en el camino caliente:** Los paquetes que llegan por IRQ no hacen `kmalloc`. Se reservan de un pool que se recicla. Los que salen también. Solo las operaciones de setup (`bind`, `accept`) asignan memoria.
3. **Sin copias:** Un paquete que llega del driver va al buffer de la app sin pasar por buffers intermedios. La única copia obligatoria es driver $\leftrightarrow$ kernel y kernel $\leftrightarrow$ userspace, como en Linux.
4. **Todo bloqueante es interrumpible:** Ningún *wait* interno del stack ignora señales. Si `read()` se bloquea esperando un paquete y llega `SIGINT`, sale con `-EINTR`.
5. **Timeouts explícitos, no "infinito":** Cada operación que puede colgarse tiene timeout. Si no se configura, un valor por defecto de la RFC (por ejemplo, TCP retransmission timeout inicial de 1 s).
6. **Estado por conexión, no global:** Dos sockets TCP no comparten buffers, timers ni nada, excepto tablas hash (protegidas con cerrojos).
7. **Tests desde el día 1:** Cada capa viene con su test unitario que se puede correr sin red física. El loopback permite testear IP/ICMP/UDP/TCP sin driver.
8. **Referencias explícitas:** Cada función lleva en comentario la RFC y la sección que implementa. Si algo se desvía de la RFC, se documenta por qué.

---

## 2. Arquitectura por capas

```text
┌─────────────────────────────────────────────────────────┐
│                     userland                            │
│   socket(AF_INET, SOCK_STREAM, 0)  →  syscall           │
└──────────────────────┬──────────────────────────────────┘
                       │  sys_socket / connect / send / recv
┌──────────────────────▼──────────────────────────────────┐
│  kernel/socket.c  —  struct socket + tabla de proto     │
│  (capa de adaptación syscall↔protocolo, como Linux)     │
└──────────────────────┬──────────────────────────────────┘
                       │  proto->ops->connect(sk, addr)
┌──────────────────────▼──────────────────────────────────┐
│  kernel/tcp.c / udp.c / raw.c / icmp.c                  │
│  (protocolos de transporte)                             │
└──────────────────────┬──────────────────────────────────┘
                       │  ip_output(skb, daddr, proto)
┌──────────────────────▼──────────────────────────────────┐
│  kernel/ip.c  —  IPv4 (fragmentación, routing básico)   │
│  kernel/icmp.c  (por encima de ip, pero en el mismo     │
│                  nivel de dispatch)                     │
└──────────────────────┬──────────────────────────────────┘
                       │  ip_send_eth(skb, next_hop)
┌──────────────────────▼──────────────────────────────────┐
│  kernel/arp.c  —  resolución IP→MAC                     │
│  kernel/route.c  —  tabla de rutas                      │
└──────────────────────┬──────────────────────────────────┘
                       │  netif->ops->transmit(netif, skb)
┌──────────────────────▼──────────────────────────────────┐
│  kernel/netif.c  —  registro de interfaces              │
└──────────────────────┬──────────────────────────────────┘
                       │  driver específico
┌──────────────────────▼──────────────────────────────────┐
│  kernel/e1000.c / loopback.c                            │
└──────────────────────┬──────────────────────────────────┘
                       │  DMA / MMIO
┌──────────────────────▼──────────────────────────────────┐
│                      hardware                           │
└─────────────────────────────────────────────────────────┘
```

### Flujo de recepción (`IRQ` $\rightarrow$ `app`)
```text
IRQ NIC  → driver lee descriptor DMA  → skb = skb_alloc_from_pool()  → skb->data = pkt  → netif_rx(skb)  → ether_type demux
    → 0x0806 → arp_input
    → 0x0800 → ip_input
                → ip_rcv
                → demux por protocolo
                  → 6  → tcp_input
                  → 17 → udp_input
                  → 1  → icmp_input
  → capa de transporte localiza el socket
  → skb se encola en sk->rx_queue
  → wake_up(&sk->rx_wq)
```

### Flujo de envío (`app` $\rightarrow$ `NIC`)
```text
write(fd) → sys_write → sock_sendmsg
  → proto->sendmsg(sk, msg)
    → skb = skb_alloc_from_pool()
    → copia datos de userspace
    → tcp_build_header / udp_build_header
    → ip_build_header (o ya viene)
    → route_lookup(daddr) → next_hop, netif
    → arp_resolve(next_hop) — puede bloquear si no está en caché
    → netif->ops->transmit(netif, skb)
    → driver programa DMA y despierta al NIC
```

---

## 3. Estructuras de datos centrales

### 3.1 `struct skb` — socket buffer
El corazón del stack. Todo paquete vive en un `skb`.

```c
typedef struct skb {
  // Buffer físico (una o varias páginas)
  void   *data;              // inicio del buffer en kernel space
  size_t  capacity;          // tamaño del buffer
  // Cabeceras. Los offsets crecen hacia abajo (head), los datos hacia arriba (tail).
  size_t  head;              // offset al inicio de la cabecera actual
  size_t  tail;              // offset al fin de los datos actuales
  // Metadatos
  uint16_t protocol;         // ETH_P_IP, ETH_P_ARP, ...
  struct netif *netif;       // interfaz de entrada/salida
  uint8_t  flags;            // SKB_SHARED, SKB_CLONED, ...
  uint16_t csum_start;       // para checksum offload (futuro)
  uint32_t csum_offset;
  // Lista (para queues)
  struct skb *next;
  // Refcount (para multicast / clone)
  int refs;
} skb_t;
```

**Operaciones:**
* `skb_alloc(size)` — reserva del pool.
* `skb_pull(skb, n)` — avanza `head` (para desencapsular).
* `skb_push(skb, n)` — retrocede `head` (para añadir cabecera).
* `skb_put(skb, n)` — avanza `tail` (para escribir payload).
* `skb_trim(skb, n)` — reduce `tail`.
* `skb_free(skb)` — devuelve al pool.
* `skb_clone(skb)` — copia solo cabecera (para multicast; `refs++`).

**Pool:** Array estático de $N$ buffers de $2\text{ KB}$ cada uno. $N = 256 \rightarrow 512\text{ KB}$ de RAM. Cola de libres, reabastecida por `kswapd` si es necesario (*swap out* de `skb`s rara vez usados).

### 3.2 `struct netif` — interfaz de red
```c
typedef struct netif {
  char      name[8];              // "lo0", "eth0"
  uint8_t   mac[6];
  uint32_t  ip;
  uint32_t  netmask;
  uint32_t  gateway;
  // Puntos de entrada del driver
  struct netif_ops *ops;
  // Estado del driver
  void     *priv;
  // Estadísticas
  uint64_t rx_packets, tx_packets;
  uint64_t rx_bytes, tx_bytes;
  uint64_t rx_errors, tx_errors;
  uint64_t rx_dropped;
  // Lista
  struct netif *next;
} netif_t;

typedef struct netif_ops {
  int (*transmit)(netif_t *netif, skb_t *skb);
  int (*open)(netif_t *netif);
  int (*close)(netif_t *netif);
  void (*get_mac)(netif_t *netif, uint8_t *out);
} netif_ops_t;
```

**API pública:**
* `netif_register(netif)` — añade a la lista.
* `netif_rx(skb)` — entrada de paquetes desde el driver.
* `netif_tx(netif, skb)` — salida hacia el driver.
* `netif_lookup(name)` / `netif_lookup_by_ip(ip)`.
* `netif_for_each(cb, arg)` — iteración segura.

### 3.3 `struct socket` — socket
Modelo Linux reducido: `struct socket` (capa de syscall) + `struct sock` (capa de protocolo).

```c
typedef struct socket {
  int              type;         // SOCK_STREAM, DGRAM, RAW
  int              state;        // SS_UNCONNECTED, SS_CONNECTED, ...
  int              flags;        // SOCK_NONBLOCK, SOCK_CLOEXEC
  struct proto_ops *ops;
  struct sock      *sk;          // estado del protocolo
  wait_queue_t     wq;           // waiters del socket
} socket_t;

struct sock {
  int              family;       // AF_INET
  int              type;
  int              protocol;
  // Direcciones
  uint32_t         saddr, daddr;
  uint16_t         sport, dport;
  // Colas
  skb_queue_t      rx_queue;     // paquetes esperando read()
  skb_queue_t      tx_queue;     // paquetes esperando ACK (TCP)
  skb_queue_t      err_queue;
  // Waiters
  wait_queue_t     rx_wq;        // despertado al llegar paquete
  wait_queue_t     tx_wq;        // despertado al poder escribir
  wait_queue_t     err_wq;
  // Timers
  uint64_t         timer_deadline;
  void             (*timer_cb)(struct sock *);
  // Refcount
  int              refs;
};
```

### 3.4 `struct proto_ops` — operaciones por protocolo
```c
typedef struct proto_ops {
  int  family;
  int  type;
  int  protocol;
  int  (*create)(socket_t *sock);
  int  (*bind)(socket_t *sock, struct sockaddr_in *addr);
  int  (*connect)(socket_t *sock, struct sockaddr_in *addr, int flags);
  int  (*listen)(socket_t *sock, int backlog);
  int  (*accept)(socket_t *sock, socket_t *newsock, int flags);
  int  (*sendmsg)(socket_t *sock, const void *buf, size_t len, int flags);
  int  (*recvmsg)(socket_t *sock, void *buf, size_t len, int flags);
  int  (*shutdown)(socket_t *sock, int how);
  int  (*close)(socket_t *sock);
  int  (*poll)(socket_t *sock, short events);
  int  (*ioctl)(socket_t *sock, unsigned long req, uint64_t arg);
  int  (*getsockname)(socket_t *sock, struct sockaddr_in *out);
  int  (*getpeername)(socket_t *sock, struct sockaddr_in *out);
  int  (*setsockopt)(socket_t *sock, int level, int opt, const void *val, size_t len);
  int  (*getsockopt)(socket_t *sock, int level, int opt, void *val, size_t *len);
} proto_ops_t;
```
**Registro:** Tabla global indexada por `(family, type)`.

---

## 4. Fases — plan detallado

Cada fase es un commit coherente y cuenta con un entregable verificable.

### Fase 0 — Infraestructura común (2 días)
* **Objetivo:** Los cimientos sobre los que se construye todo.
* **Ficheros nuevos:**
  * `kernel/net/skb.c` + `skb.h`
  * `kernel/net/netif.c` + `netif.h`
  * `kernel/net/byteorder.h` (helpers `htonl`, `ntohs`, ...)
  * `kernel/net/checksum.c` (checksum IP, UDP, TCP)
* **Tareas:** `skb_alloc`/`free`, operaciones `pull`/`push`/`put`/`trim`/`clone`, `netif_register`/`lookup`, `netif_rx` (dispatch por EtherType), helpers de byte order, Checksums (IP 16-bit one's complement, UDP/TCP pseudo-header checksum, test vectors de la RFC 1071).
* **Test unitario:** `test_skb.c` en `kernel_tests`: `skb_alloc`/`free` 1000 veces sin fugas, `skb_push`/`pull` manteniendo coherentes `head`/`tail`, checksum de vectores conocidos.
* **Entregable verificable:** Los tests pasan; `netif_rx` con un paquete de prueba reenvía a un handler dummy.

### Fase 1 — Loopback (1 día)
* **Objetivo:** `127.0.0.1` funciona sin driver físico.
* **Ficheros nuevos:** `kernel/net/loopback.c`
* **Tareas:** Registrar `lo0` con IP `127.0.0.1/8`, MAC `00:00:00:00:00:00`. `lo0_transmit` $\rightarrow$ llama a `netif_rx` directamente (sin DMA, sin IRQ). Routing: cualquier `127.x.x.x` va a `lo0`.
* **Test unitario:** `netif_tx(lo0, skb)` $\rightarrow$ `netif_rx` recibe el mismo `skb`. Paquete malformado (checksum inválido) se descarta en `ip_input`.
* **Entregable verificable:** Test del kernel que envía un ICMP echo request a `127.0.0.1` y recibe el reply.

### Fase 2 — ARP + IPv4 + ICMP (3 días)
* **Objetivo:** Ping a `127.0.0.1` y ping a gateway funcionan.
* **Ficheros nuevos:** `arp.c/.h`, `ip.c/.h`, `icmp.c/.h`, `route.c/.h`.
* **Tareas:** 
  * **ARP:** Caché IP $\rightarrow$ MAC (`ip`, `mac`, `state`, `expiry`), `arp_resolve`, `arp_input`, timeout de entrada (30 s).
  * **IPv4:** `ip_input` (valida checksum, versión, IHL, longitud), demux por protocolo, `ip_output`.
  * **ICMP:** Echo request/reply, Destination unreachable, Time exceeded.
  * **Route:** Tabla de rutas, `route_lookup(daddr)`, ruta por defecto, ruta directa.
* **Entregable verificable:** Ping `127.0.0.1` end-to-end (kernel $\rightarrow$ kernel) y comando `ping` rudimentario en userspace.

### Fase 3 — Driver e1000 (3 días)
* **Objetivo:** Tráfico Ethernet real en QEMU.
* **Ficheros nuevos:** `kernel/net/e1000.c` + `e1000.h`
* **Tareas:** Detección PCI (vendor `0x8086`, device `0x100E`), registros MMIO, setup de anillos RX/TX (256 descriptores), IRQ handler, `e1000_transmit`, reset del NIC, autonegociación.
* **Entregable verificable:** Ping `10.0.2.2` (gateway QEMU). ARP resuelve la MAC del gateway, ICMP echo reply.

### Fase 4 — Sockets + UDP (3 días)
* **Objetivo:** `nc -u` funciona. Python `socket.socket(AF_INET, SOCK_DGRAM)` funciona.
* **Ficheros nuevos:** `socket.c/.h`, `udp.c/.h`, `addr.c`.
* **Modificaciones a `syscall.c`:** Implementar `k_socket`, `k_bind`, `k_connect`, `k_sendto`, `k_recvfrom`, etc.
* **Entregable verificable:** `nc -u -l 12345` en un shell, `echo hola | nc -u 127.0.0.1 12345` en otro. Se ve "hola".

### Fase 5 — TCP (1-2 semanas)
* **Objetivo:** `curl` contra un servidor HTTP local funciona.
* **Ficheros nuevos:** `tcp.c/.h`, `tcp_state.c`, `tcp_timer.c`.
* **Tareas (sub-fases):** Handshake, transferencia con ventana deslizante, retransmisión con RTO (RFC 6298), cierre FIN, control de congestión (RFC 5681), Delayed ACK + Nagle.
* **Entregable verificable:** `python3 -m http.server 8080` en Aurora, `curl http://127.0.0.1:8080/file` obtiene el fichero.

### Fase 6 — DNS (2 días)
* **Objetivo:** `curl http://example.com` funciona (con gateway a la salida).
* **Tareas:** Configurar `getaddrinfo` en glibc mediante `/etc/resolv.conf` y queries UDP nativas.
* **Entregable verificable:** `curl http://example.com` devuelve HTML.

### Fase 7 — Features avanzadas (Opcional, 1-2 semanas)
* DHCP client, IPv6, SACK, Window scaling, TCP timestamps, Path MTU discovery, Multicast, Raw sockets, Netfilter/iptables.

---

## 5. Cronograma realista

| Fase | Descripción | Días | Acumulado |
| :--- | :--- | :--- | :--- |
| **0** | Infraestructura | 2 | 2 |
| **1** | Loopback | 1 | 3 |
| **2** | ARP+IP+ICMP | 3 | 6 |
| **3** | Driver e1000 | 3 | 9 |
| **4** | Sockets+UDP | 3 | 12 |
| **5** | TCP | 10-15 | 22-27 |
| **6** | DNS | 2 | 24-29 |
| **7** | Opcional | Varios | — |

**Total MVP (fases 0-6):** 4 a 6 semanas de trabajo real (~15-20 sesiones de chat).

---

## 6. Riesgos y mitigaciones

* **Driver e1000 no funciona en el primer intento (Alta):** Verificar con QEMU + log detallado. Benchmarks de loopback interno.
* **TCP se cuelga en el handshake (Alta):** Instrumentación desde el día 1. `tcpdump` en el guest desde fase 2.
* **Checksums mal calculados (Alta):** Test vectors de la RFC 1071. Checksum offload deshabilitado al principio.
* **Condiciones de carrera en colas de socket (Media):** Un lock por socket (`sk->lock`). Cero locks globales.
* **Consumo de memoria descontrolado (Media):** Pool de `skb`s con cota dura y comandos estilo `netstat`.

---

## 7. Lo que NO vamos a hacer (Alcance inicial)

* No implementaremos TCP SACK, timestamps ni window scaling en la v1.
* No soportaremos IPv6 en v1.
* No implementaremos IPsec, VPN o encapsulaciones avanzadas.
* No haremos un firewall completo (`netfilter`).
* No paralelizaremos el stack: un solo hilo de proceso de paquetes por interfaz.
* No haremos *checksum offload* en la v1.

---

## 8. Referencias y Bibliografía

* **RFCs:** RFC 791 (IPv4), RFC 792 (ICMP), RFC 793 (TCP), RFC 768 (UDP), RFC 826 (ARP), RFC 1071 (Checksum), RFC 1122 (Host requirements), RFC 5681 (Congestion control), RFC 6298 (RTO).
* **Libros:** *TCP/IP Illustrated, Volume 1* (Stevens); *Unix Network Programming, Volume 1* (Stevens).
* **Código de referencia:** lwIP (BSD), Linux `net/ipv4/`, FreeBSD `sys/netinet/`.

---

## 9. Estructura de directorios

```text
kernel/net/
├── byteorder.h
├── checksum.c
├── checksum.h
├── skb.c
├── skb.h
├── netif.c
├── netif.h
├── loopback.c
├── arp.c
├── arp.h
├── route.c
├── route.h
├── ip.c
├── ip.h
├── icmp.c
├── icmp.h
├── udp.c
├── udp.h
├── tcp.c
├── tcp.h
├── socket.c
├── socket.h
├── addr.c
├── addr.h
├── e1000.c
├── e1000.h
└── net_tests.c           (tests del kernel)
```

---

## 10. Cambios a syscall.c

Syscalls a implementar (todas con número Linux x86_64):
* `socket` (41), `connect` (42), `accept` (43), `sendto` (44), `recvfrom` (45), `sendmsg` (46), `recvmsg` (47), `shutdown` (48), `bind` (49), `listen` (50), `getsockname` (51), `getpeername` (52), `socketpair` (53 - AF_UNIX; por ahora `-EAFNOSUPPORT`), `setsockopt` (54), `getsockopt` (55).

---

## 11. Criterios de éxito por fase

* **Fase 0:** `test_skb` pasa. `test_checksum` con vectores RFC.
* **Fase 1:** Kernel test envía ICMP echo a `127.0.0.1`, recibe reply.
* **Fase 2:** Ping `127.0.0.1` desde userspace funciona.
* **Fase 3:** Ping `10.0.2.2` funciona. `ip link` muestra `eth0` con MAC real.
* **Fase 4:** `nc -u` end-to-end con loopback.
* **Fase 5:** `python3 -m http.server` + `curl http://127.0.0.1:8080/` funciona.
* **Fase 6:** `curl http://example.com` devuelve HTML real.
* **Fase 7:** Cada feature adicional con su test dedicado.

---

## 12. Cuándo paramos

El MVP del stack cubre las **fases 0 a 5**. Con ello tendrás una red funcional en QEMU, sockets `AF_INET` y UDP+TCP completos en su camino feliz. La **fase 6 (DNS)** abre el acceso al mundo real, mientras que la **fase 7** queda para mejoras incrementales posteriores.