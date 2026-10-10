// kernel/net/udp.h
//
// UDP (RFC 768). Sin sockets: dispatch por puerto a callbacks
// registrados. La capa de sockets llega en Fase 6b.

#ifndef KERNEL_NET_UDP_H
#define KERNEL_NET_UDP_H

#include "skb.h"
#include <stdint.h>

#define UDP_HDR_LEN 8
#define UDP_PORT_MAX 64

// El skb que recibe el handler tiene el header UDP ya quitado:
// skb_data() apunta al payload, skb_len() es la longitud del payload.
// El handler es responsable de consumir el skb (free o reuso).
typedef void (*udp_handler_t)(skb_t *skb, uint32_t src_ip, uint16_t src_port,
                              uint32_t dst_ip, uint16_t dst_port);

// Inicializa el módulo y registra el protocolo 17 en IP.
void udp_init(void);

// Registra (o reemplaza) un handler para un puerto. 0 < port.
// Devuelve 0 si OK, -EINVAL/-ENOMEM.
int udp_register_port(uint16_t port, udp_handler_t handler);

// Desregistra el puerto. Devuelve 0 si existía, -ENOENT si no.
int udp_unregister_port(uint16_t port);

// Envía `skb` (que contiene SOLO el payload) como UDP.
//   src_ip != 0 (obligatorio: pseudo-header del checksum).
//   src_port != 0.
//   dst_ip en host order.
// En éxito el skb está consumido por ip_output. En error, udp_output
// lo libera.
int udp_output(skb_t *skb, uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
               uint16_t dst_port);

#endif