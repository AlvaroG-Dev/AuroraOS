// kernel/net/tcp.h
//
// TCP (RFC 793). Fase 7a: parseo, checksum y tabla de conexiones.
// El state machine se añade en Fase 7b.

#ifndef KERNEL_NET_TCP_H
#define KERNEL_NET_TCP_H

#include "../wait.h"
#include "skb.h"
#include <stddef.h>
#include <stdint.h>

#define TCP_HDR_MIN_LEN 20
#define TCP_HDR_MAX_LEN 60
#define TCP_ACCEPT_MAX 8 // [Fase 7d] cola de conexiones aceptadas por listener

// Flags (9 bits bajos de offset 12-13).
#define TCP_FIN 0x001
#define TCP_SYN 0x002
#define TCP_RST 0x004
#define TCP_PSH 0x008
#define TCP_ACK 0x010
#define TCP_URG 0x020
#define TCP_ECE 0x040
#define TCP_CWR 0x080
#define TCP_NS 0x100

// Estados de la conexión. Fase 7b+ los usa; en 7a solo CLOSED/LISTEN.
enum tcp_state {
  TCP_STATE_CLOSED = 0,
  TCP_STATE_LISTEN,
  TCP_STATE_SYN_SENT,
  TCP_STATE_SYN_RECEIVED,
  TCP_STATE_ESTABLISHED,
  TCP_STATE_FIN_WAIT_1,
  TCP_STATE_FIN_WAIT_2,
  TCP_STATE_CLOSE_WAIT,
  TCP_STATE_CLOSING,
  TCP_STATE_LAST_ACK,
  TCP_STATE_TIME_WAIT,
};

// Header parseado.
typedef struct {
  uint16_t sport;
  uint16_t dport;
  uint32_t seq;
  uint32_t ack;
  uint8_t data_offset; // en palabras de 4 bytes
  uint16_t flags;      // bits TCP_*
  uint16_t window;
  uint16_t csum;
  uint16_t urgent;
  size_t hdr_len; // data_offset * 4
} tcp_hdr_t;

// Parsea un segmento TCP (sin Ethernet, sin IP). Devuelve 0 si OK,
// -EINVAL si mal formado (truncado, data_offset < 5, hdr_len > seg_len).
int tcp_parse_header(const uint8_t *seg, size_t seg_len, tcp_hdr_t *out);

// Verifica checksum con pseudo-header IPv4. 0 si OK, -1 si mal.
int tcp_checksum_verify(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg,
                        size_t seg_len);

// Calcula checksum. El llamante debe haber puesto seg[16..17] = 0.
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg,
                      size_t seg_len);

// Inicializa el módulo y registra IPPROTO_TCP en ip.
void tcp_init(void);

// ---------------------------------------------------------------------------
// Conexiones. Struct público para simplicidad en 7a; si en 7b+ hace falta
// encapsular, se añaden accessors y se hace opaco.
// ---------------------------------------------------------------------------
typedef struct tcp_conn tcp_conn_t;

// Handler llamado cuando llega un segmento para una conexión existente.
// El skb tiene el header TCP NO quitado (el handler decide si lo quita).
// El handler es responsable de consumir el skb (free o reuso).
typedef void (*tcp_conn_handler_t)(tcp_conn_t *c, skb_t *skb,
                                   const tcp_hdr_t *hdr, uint32_t src_ip,
                                   uint32_t dst_ip);

struct tcp_conn {
  enum tcp_state state;
  uint32_t local_ip;
  uint16_t local_port;
  uint32_t remote_ip;
  uint16_t remote_port;
  tcp_conn_handler_t handler;
  struct tcp_conn *next;

  uint32_t snd_isn;
  uint32_t snd_una;
  uint32_t snd_nxt;
  uint32_t rcv_nxt;

  wait_queue_t handshake_wq;
  int handshake_ok;

  // [Fase 7c] Cola RX de datos ya ACKeadeados. El consumidor (test o
  // socket) hace tcp_recv() y saca de aquí.
  wait_queue_t rx_wq;
  skb_t *rx_head;
  skb_t *rx_tail;
  int rx_count;

  // [Fase 7d] LISTEN y cola de aceptados.
  int is_listener;              // 1 si state == LISTEN
  wait_queue_t accept_wq;       // despierta a tcp_accept
  struct tcp_conn *accept_head; // cola FIFO de hijos ESTABLISHED
  struct tcp_conn *accept_tail;
  struct tcp_conn *accept_next;     // enlace en la cola del listener
  struct tcp_conn *parent_listener; // NULL si no vino de un listener
  int accept_count;

  int peer_closed; // [7f] 1 = el peer ya envió FIN
};

// Crea una conexión en TCP_STATE_CLOSED con todos los campos a 0.
// Devuelve NULL si la tabla está llena.
tcp_conn_t *tcp_conn_alloc(void);

// Devuelve la conexión a la tabla libre (no la quita del hash; usar
// tcp_conn_remove antes si está insertada).
void tcp_conn_free(tcp_conn_t *c);

// Inserta en el hash. El llamante debe haber rellenado el 4-tuple.
// Devuelve 0 si OK, -EEXIST si ya existe una idéntica, -EINVAL si
// algún campo está a 0.
int tcp_conn_insert(tcp_conn_t *c);

// Quita del hash. Idempotente.
void tcp_conn_remove(tcp_conn_t *c);

// Busca por (local_ip, local_port, remote_ip, remote_port).
// Devuelve NULL si no existe.
tcp_conn_t *tcp_conn_lookup(uint32_t lip, uint16_t lp, uint32_t rip,
                            uint16_t rp);

// Vacía la tabla (para tests y para reconfiguración).
void tcp_flush(void);

// ---------------------------------------------------------------------------
// [Fase 7b] Handshake activo.
// ---------------------------------------------------------------------------

// Conecta a (remote_ip, remote_port). Bloquea hasta que la conexión
// está ESTABLISHED, hasta `timeout_ms`, o hasta error.
//   - El socket ya debe estar bound (local_ip/local_port rellenos).
//   - Devuelve 0 si OK, -ETIMEDOUT/-EINVAL/-ENOMEM si falla.
// La conexión queda insertada en la tabla hash y lista para send/recv
// (que llegarán en 7c).
int tcp_connect(tcp_conn_t *c, uint32_t remote_ip, uint16_t remote_port,
                uint64_t timeout_ms);

// ---------------------------------------------------------------------------
// [Fase 7c] Envío y recepción
// ---------------------------------------------------------------------------

// Envía `len` bytes como uno o más segmentos TCP. Un solo segmento en
// esta fase (no fragmentamos por MSS).
//   - c debe estar ESTABLISHED.
//   - Bloqueante hasta que se copia al skb y sale por la NIC.
// Devuelve `len` si OK, <0 si error.
int tcp_send(tcp_conn_t *c, const void *buf, size_t len);

// Recibe hasta `len` bytes de la cola RX. Si no hay datos:
//   - timeout_ms == 0    → bloquea indefinidamente.
//   - timeout_ms >  0    → bloquea hasta timeout_ms.
// Devuelve >0 bytes copiados, 0 si el peer cerró, -ETIMEDOUT/-EINTR.
int tcp_recv(tcp_conn_t *c, void *buf, size_t len, uint64_t timeout_ms);

// ---------------------------------------------------------------------------
// [Fase 7d] Servidor pasivo: listen / accept
// ---------------------------------------------------------------------------

// Pone la conexión en LISTEN sobre (local_ip, local_port).
//   local_ip == 0 → se autoselecciona del primer netif con IP != 0.
// La conexión queda insertada en la tabla hash y lista para aceptar SYN.
// Devuelve 0 si OK, <0 si error.
int tcp_listen(tcp_conn_t *c, uint32_t local_ip, uint16_t local_port);

// Bloquea hasta que haya una conexión aceptada en la cola del listener.
//   timeout_ms == 0 → devuelve NULL inmediatamente si no hay ninguna.
//   timeout_ms >  0 → espera hasta timeout_ms.
// Devuelve la conn hija en ESTABLISHED (el llamante la posee: debe
// llamar a tcp_conn_remove + tcp_conn_free cuando termine), o NULL si
// no hay / timeout.
tcp_conn_t *tcp_accept(tcp_conn_t *listener, uint64_t timeout_ms);

// [Fase 7e] Cierra una conexión TCP: envía FIN si está ESTABLISHED y
// libera la conn. MVP sin esperar ACK del FIN (Fase 7f añadirá
// TIME_WAIT y demás). Idempotente con NULL.
void tcp_close(tcp_conn_t *c);

// [Fase 7e] Igual que tcp_recv pero sin bloquear: si no hay datos,
// devuelve -EAGAIN inmediatamente.
int tcp_recv_nonblock(tcp_conn_t *c, void *buf, size_t len);

// [7f] ¿El peer ya cerró su lado? Tras esto, tcp_recv devolverá 0.
int tcp_peer_closed(const tcp_conn_t *c);

#endif