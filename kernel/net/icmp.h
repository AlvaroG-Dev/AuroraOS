// kernel/net/icmp.h
//
// ICMP mínimo: echo request / echo reply. Sin mensajes de error
// (destination unreachable, time exceeded, ...). Eso vendrá cuando
// haya forwarding o cuando implementemos traceroute.

#ifndef KERNEL_NET_ICMP_H
#define KERNEL_NET_ICMP_H

#include "skb.h"
#include <stddef.h>
#include <stdint.h>

#define ICMP_TYPE_ECHO_REPLY 0
#define ICMP_TYPE_ECHO_REQUEST 8

#define ICMP_HDR_LEN 8

// Callback para replies a nuestros echo requests.
// Filtra por ident en el llamante.
typedef void (*icmp_echo_reply_fn)(uint32_t src_ip, uint16_t ident,
                                   uint16_t seq, const uint8_t *data,
                                   size_t data_len);

// Inicializa el módulo y registra el handler del protocolo IP 1.
void icmp_init(void);

// Instala un callback de echo reply. Hasta 4 en paralelo (los usa
// kernel ping y los ping sockets de userland a la vez).
// Pasar NULL limpia TODOS los callbacks.
// Devuelve 0 si OK, -ENOMEM si la tabla está llena.
int icmp_set_echo_reply_cb(icmp_echo_reply_fn fn);

// Desinstala un callback concreto. Idempotente.
void icmp_remove_echo_reply_cb(icmp_echo_reply_fn fn);

// Envía un echo request. dst_ip en host order. Devuelve 0 si OK.
int icmp_echo_request(uint32_t dst_ip, uint16_t ident, uint16_t seq,
                      const void *data, size_t data_len);

// Capa de bajo nivel: el skb contiene SOLO el payload del ICMP
// (sin header). icmp_output añade los 8 bytes del header. El skb se
// consume siempre (éxito o error).
int icmp_output(skb_t *skb, uint32_t dst_ip, uint8_t type, uint8_t code,
                uint16_t ident, uint16_t seq);

#endif