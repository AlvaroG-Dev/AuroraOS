// kernel/net/tcp.h
//
// TCP (RFC 793) + extensiones:
//   - Window scaling (RFC 7323)
//   - Congestion control (RFC 5681): slow start + congestion avoidance
//   - RTO dinámico (RFC 6298): Jacobson/Karels
//   - Fast retransmit + fast recovery (NewReno, RFC 6582)
//   - Autotune de la ventana de recepción (basado en rate × RTT)
//   - Cola de retransmisión con timers

#ifndef KERNEL_NET_TCP_H
#define KERNEL_NET_TCP_H

#include "../spinlock.h"
#include "../wait.h"
#include "skb.h"
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constantes del protocolo
// ---------------------------------------------------------------------------
#define TCP_HDR_MIN_LEN 20
#define TCP_HDR_MAX_LEN 60
#define TCP_ACCEPT_MAX 8

// MSS para TAP + MTU 1500. Con SLiRP usa 1300.
#define TCP_RX_MSS 1460

// Buffer máximo del kernel por conexión. Techo que autotune puede alcanzar.
#define TCP_RX_BUFFER_BYTES (4u * 1024u * 1024u) // 4 MB
#define TCP_RX_MAX_SKBS 4096                     // 4096 × 1460 = 5.8 MB

// Autotune: ventana inicial pequeña para arrancar; crece en función del
// rate medido × RTT. Nunca baja del mínimo. Nunca sube del techo.
#define TCP_RX_WINDOW_INIT (256u * 1024u)   // 256 KB al arrancar (Bridge necesita más BDP)
#define TCP_RX_WINDOW_MIN (8u * TCP_RX_MSS) // 8 MSS ≈ 11.7 KB

// Shift de window scaling. Con 4 MB >> 7 = 32768, cabe en uint16.
#define TCP_WSCALE_SHIFT 7
#define TCP_PEER_WINDOW_MAX (4u * 1024u * 1024u)

// RTO bounds (RFC 6298). Floor bajo para que TAP reaccione rápido.
#define TCP_RTO_MIN_MS 500ULL   // 500 ms: redes físicas (Bridge) tienen latencia > 200 ms
#define TCP_RTO_MAX_MS 60000ULL
#define TCP_RTO_INITIAL_MS 1000ULL

// Umbral de dup ACKs para fast retransmit (RFC 5681).
#define TCP_DUP_ACK_THRESHOLD 3

// Autotune: sampling cada N ms.
#define TCP_AUTOTUNE_SAMPLE_MS 500ULL  // 500 ms: ventana de medición más estable

// Autotune: RTT mínimo usado en la fórmula. Con RTT real de 0.1 ms (local)
// la fórmula daría un target minúsculo; forzamos 20 ms como floor.
#define TCP_AUTOTUNE_RTT_FLOOR_MS 50  // 50 ms floor: previene ventana minúscula con RTT bajo

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
#define TCP_FIN 0x001
#define TCP_SYN 0x002
#define TCP_RST 0x004
#define TCP_PSH 0x008
#define TCP_ACK 0x010
#define TCP_URG 0x020
#define TCP_ECE 0x040
#define TCP_CWR 0x080
#define TCP_NS 0x100

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

typedef struct {
  uint16_t sport;
  uint16_t dport;
  uint32_t seq;
  uint32_t ack;
  uint8_t data_offset;
  uint16_t flags;
  uint16_t window;
  uint16_t csum;
  uint16_t urgent;
  size_t hdr_len;
} tcp_hdr_t;

int tcp_parse_header(const uint8_t *seg, size_t seg_len, tcp_hdr_t *out);
int tcp_checksum_verify(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg,
                        size_t seg_len);
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg,
                      size_t seg_len);
void tcp_init(void);

typedef struct tcp_conn tcp_conn_t;

typedef void (*tcp_conn_handler_t)(tcp_conn_t *c, skb_t *skb,
                                   const tcp_hdr_t *hdr, uint32_t src_ip,
                                   uint32_t dst_ip);

// ---------------------------------------------------------------------------
// Estado por conexión
// ---------------------------------------------------------------------------
struct tcp_conn {
  enum tcp_state state;
  uint32_t local_ip;
  uint16_t local_port;
  uint32_t remote_ip;
  uint16_t remote_port;
  tcp_conn_handler_t handler;
  struct tcp_conn *next;

  // Espacio de secuencia
  uint32_t snd_isn;
  uint32_t snd_una; // primer byte sin ACK
  uint32_t snd_nxt; // próximo byte a enviar
  uint32_t rcv_nxt; // próximo byte esperado del peer

  // Handshake
  wait_queue_t handshake_wq;
  int handshake_ok;
  uint64_t syn_sent_tick; // para medir RTT inicial

  // Cola de RX
  wait_queue_t rx_wq;
  spinlock_t rx_lock;
  skb_t *rx_head;
  skb_t *rx_tail;
  int rx_count;
  skb_t *ooo_head; // out-of-order
  int ooo_count;
  size_t rx_buffered_bytes;
  uint16_t rcv_last_window; // último wire window anunciado
  uint32_t pending_fin_seq;
  int pending_fin;

  // Listener
  int is_listener;
  wait_queue_t accept_wq;
  struct tcp_conn *accept_head;
  struct tcp_conn *accept_tail;
  struct tcp_conn *accept_next;
  struct tcp_conn *parent_listener;
  int accept_count;

  int peer_closed;

  // MSS del peer (SYN o SYN-ACK)
  uint16_t peer_mss;

  // Ventana del peer (send window para nosotros) YA ESCALADA
  uint32_t peer_window;
  uint64_t peer_window_tick;

  // Congestión
  uint32_t cwnd;
  uint32_t ssthresh;

  // WSCALE negociado (RFC 7323)
  uint8_t snd_wscale; // shift a aplicar a las ventanas del peer
  uint8_t rcv_wscale; // shift que anunciamos
  uint8_t wscale_negotiated;
  uint8_t _pad_wscale;

  // Cola de retransmisión
  skb_t *rtx_head;
  skb_t *rtx_tail;
  int rtx_count;
  int rtx_consec_retries;
  int rtx_total_retries;
  uint64_t last_ack_tick;

  int pending_window_update;

  // [AUTOTUNE] Ventana de recepción dinámica
  uint32_t rcv_window_target; // lo que anunciamos ahora
  uint32_t rcv_window_max;    // techo
  uint32_t rcv_window_min;    // suelo
  uint64_t rcv_sample_tick;   // último sample
  uint64_t rcv_sample_bytes;  // bytes entregados desde el último sample

  // [RTT] Estimación de RTT (Jacobson/Karels)
  uint32_t srtt_ms;        // Smoothed RTT
  uint32_t rttvar_ms;      // Variación
  uint32_t rto_ms;         // RTO calculado
  int rtt_measured;        // 1 si srtt ya tiene un sample
  uint32_t rtt_probe_seq;  // último end_seq enviado con payload
  uint64_t rtt_probe_tick; // tick en que se envió
  int rtt_probe_outstanding;

  // [FAST RETRANSMIT] Detección de dup ACKs
  uint32_t dup_ack_count;
  int in_fast_recovery;
  uint32_t recover_seq; // seq hasta el que estamos en fast recovery

  // Trazas (una vez por conexión)
  int trace_tx_payload_logged;
  int trace_rx_payload_logged;
  int trace_rx_app_logged;
  int trace_ack_error_logged;
  int trace_rx_empty_wait_logged;
  uint64_t rx_total_queued;
  uint64_t rx_total_consumed;
  uint64_t rate_ewma_kbps; // [AUTOTUNE] rate suavizado (KB/s)
};

tcp_conn_t *tcp_conn_alloc(void);
void tcp_conn_free(tcp_conn_t *c);
int tcp_conn_insert(tcp_conn_t *c);
void tcp_conn_remove(tcp_conn_t *c);
tcp_conn_t *tcp_conn_lookup(uint32_t lip, uint16_t lp, uint32_t rip,
                            uint16_t rp);
void tcp_flush(void);

int tcp_connect(tcp_conn_t *c, uint32_t remote_ip, uint16_t remote_port,
                uint64_t timeout_ms);
int tcp_send(tcp_conn_t *c, const void *buf, size_t len);
int tcp_recv(tcp_conn_t *c, void *buf, size_t len, uint64_t timeout_ms);
int tcp_listen(tcp_conn_t *c, uint32_t local_ip, uint16_t local_port);
tcp_conn_t *tcp_accept(tcp_conn_t *listener, uint64_t timeout_ms);
void tcp_close(tcp_conn_t *c);
int tcp_recv_nonblock(tcp_conn_t *c, void *buf, size_t len);
int tcp_is_readable(tcp_conn_t *c);
int tcp_peer_closed(tcp_conn_t *c);

void tcp_timer_tick(void);

#endif