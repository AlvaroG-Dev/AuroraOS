// kernel/net/socket.h
//
// Sockets BSD mínimo: solo AF_INET, SOCK_DGRAM (UDP). TCP y RAW
// llegarán después. Cada socket vive en la tabla global indexado por
// (proto, local_port), y se expone a userland mediante un vfs_node_t
// con ops propias (ver inet_socket.c).

#ifndef KERNEL_NET_SOCKET_H
#define KERNEL_NET_SOCKET_H

#include "../wait.h"
#include "skb.h"
#include <stddef.h>
#include <stdint.h>

// Socket types (Linux).
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3

// Address families.
#define AF_INET 2
#define AF_INET6 10
#define AF_UNSPEC 0

// SOCK_* flags que viajan en el argumento `type`.
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000

// Protocolo especial: 0 → autoseleccionar según type.
#define SOCK_PROTO_AUTO 0

// IPPROTO_* relevantes.
#define SOCK_IPPROTO_ICMP 1
#define SOCK_IPPROTO_TCP 6
#define SOCK_IPPROTO_UDP 17

#define SOCK_ADDR_ANY 0x00000000u

void socket_subsystem_init(void);

typedef struct socket socket_t;

// Crea un socket. Devuelve NULL si type/proto no soportado o sin memoria.
socket_t *socket_create(int type, int protocol);

// Incrementa/decrementa refcount. socket_put destruye si llega a 0.
void socket_ref(socket_t *s);
void socket_put(socket_t *s);

// Bind a (ip, port). ip==SOCK_ADDR_ANY → IP local autodetectada.
// port==0 → efímero.
int socket_bind(socket_t *s, uint32_t ip, uint16_t port);

// Connect solo relevante para UDP como "recordar el peer".
int socket_connect(socket_t *s, uint32_t ip, uint16_t port);

// send/recv. `would_block` se pone a 1 si el socket es non-blocking
// y la operación no puede completarse sin bloquear.
int socket_send(socket_t *s, const void *buf, size_t len);
int socket_recv(socket_t *s, void *buf, size_t len, int *would_block);
int socket_sendto(socket_t *s, const void *buf, size_t len, uint32_t dst_ip,
                  uint16_t dst_port);
int socket_recvfrom(socket_t *s, void *buf, size_t len, uint32_t *src_ip,
                    uint16_t *src_port, int *would_block);

// Devuelve el fd local del socket (asignado en bind) o 0.
uint16_t socket_local_port(const socket_t *s);
uint32_t socket_local_ip(const socket_t *s);
uint32_t socket_remote_ip(const socket_t *s);
uint16_t socket_remote_port(const socket_t *s);
int socket_is_connected(const socket_t *s);

// Para el handler UDP: encola un skb (payload ya limpio).
void socket_rx_enqueue(socket_t *s, skb_t *skb);

// Marca el socket como no-bloqueante (SOCK_NONBLOCK).
void socket_set_nonblock(socket_t *s, int nb);

// Uso interno de inet_socket.c. No exportar.
int socket_is_nonblock(const socket_t *s);

// Devuelve la wait queue de RX del socket. Pensado para VFS/select/poll,
// que necesitan dormir hasta que llegue un paquete sin conocer los
// internals del struct.
struct wait_queue *socket_rx_wq(socket_t *s);

// Timeout por defecto para recv, en ms. 0 = sin timeout (bloqueo
// indefinido). Lo configura SO_RCVTIMEO.
void socket_set_rcvtimeo(socket_t *s, uint64_t ms);
uint64_t socket_get_rcvtimeo(const socket_t *s);

#endif