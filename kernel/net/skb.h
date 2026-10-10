// kernel/net/skb.h
//
// Socket buffer. Todo paquete del stack de red vive aquí. Pool
// estático de SKB_POOL_SIZE skbs de SKB_BUF_SIZE bytes cada uno.
//
// El buffer tiene dos "cabezas":
//   [head_buf ... | headroom | HEAD | data | TAIL | tailroom ... end]
//
// head y tail son OFFSETS dentro del buffer. data() apunta a
// head_buf+head, len() = tail - head.
//
// Al recibir un paquete: el driver escribe el frame completo en
// data(), luego skb_pull() para quitar cabeceras según se
// desencapsula.
//
// Al enviar: el protocolo usa skb_push() para prepend cabeceras. El
// buffer tiene SKB_DEFAULT_HEADROOM bytes reservados al inicio para
// que esto funcione sin realloc.

#ifndef KERNEL_NET_SKB_H
#define KERNEL_NET_SKB_H

#include <stddef.h>
#include <stdint.h>

struct netif;

#define SKB_POOL_SIZE 256
#define SKB_BUF_SIZE 2048
#define SKB_DEFAULT_HEADROOM 64

// EtherTypes (RFC 894). Ya en network order (big-endian) como
// aparece en el wire. Los compara con skb->protocol que también
// está en network order.
#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_IPV6 0x86DD

typedef struct skb {
  uint8_t *head_buf;  // buffer físico del pool
  uint16_t head;      // offset del inicio de cabecera actual
  uint16_t tail;      // offset del fin de datos actuales
  uint16_t end;       // capacidad del buffer (= SKB_BUF_SIZE)
  uint16_t protocol;  // EtherType, network order
  uint8_t dst_mac[6]; // MAC destino (para netif_tx)
  uint8_t flags;
  uint8_t _pad;
  struct netif *netif;
  struct skb *next; // para colas
} skb_t;

// Inicializa el pool. Llamar una vez en net_init().
void skb_init(void);

// Toma un skb del pool. Devuelve NULL si está vacío.
// El buffer NO está a cero. head=SKB_DEFAULT_HEADROOM, tail=head.
skb_t *skb_alloc(void);

// Devuelve el skb al pool.
void skb_free(skb_t *skb);

// ---------------------------------------------------------------------------
// Operaciones inline sobre el skb
// ---------------------------------------------------------------------------

// Datos actuales (payload + cabeceras).
static inline uint8_t *skb_data(skb_t *s) { return s->head_buf + s->head; }

// Longitud de los datos actuales.
static inline size_t skb_len(const skb_t *s) {
  return (size_t)(s->tail - s->head);
}

// Headroom disponible (por delante de head).
static inline size_t skb_headroom(const skb_t *s) { return s->head; }

// Tailroom disponible (por detrás de tail).
static inline size_t skb_tailroom(const skb_t *s) { return s->end - s->tail; }

static inline int skb_is_empty(const skb_t *s) { return s->head == s->tail; }

// Reserva `n` bytes de headroom. Solo válido en un skb recién
// allocado, antes de escribir datos.
static inline int skb_reserve(skb_t *s, size_t n) {
  if (s->head + n > s->end)
    return -1;
  s->head += (uint16_t)n;
  s->tail = s->head;
  return 0;
}

// Añade `n` bytes al final de los datos. Devuelve un puntero a la
// región nueva (donde el llamante escribirá), o NULL si no cabe.
static inline void *skb_put(skb_t *s, size_t n) {
  if (s->tail + n > s->end)
    return NULL;
  void *p = s->head_buf + s->tail;
  s->tail += (uint16_t)n;
  return p;
}

// Retrocede head `n` bytes (para prepend). Devuelve el nuevo
// comienzo de datos, o NULL si no hay headroom.
static inline void *skb_push(skb_t *s, size_t n) {
  if (s->head < n)
    return NULL;
  s->head -= (uint16_t)n;
  return s->head_buf + s->head;
}

// Avanza head `n` bytes (para desencapsular). Devuelve el nuevo
// comienzo, o NULL si n > len.
static inline void *skb_pull(skb_t *s, size_t n) {
  if (n > skb_len(s))
    return NULL;
  s->head += (uint16_t)n;
  return s->head_buf + s->head;
}

// Reduce tail `n` bytes.
static inline void skb_trim(skb_t *s, size_t n) {
  if (n > skb_len(s))
    return;
  s->tail -= (uint16_t)n;
}

// Reinicia el skb a estado recién allocado (mantiene head_buf).
static inline void skb_reset(skb_t *s) {
  s->head = SKB_DEFAULT_HEADROOM;
  s->tail = s->head;
  s->protocol = 0;
  s->flags = 0;
  s->netif = NULL;
  s->next = NULL;
}

// Estadísticas (para debug)
int skb_pool_used(void);
int skb_pool_free_count(void);

#endif