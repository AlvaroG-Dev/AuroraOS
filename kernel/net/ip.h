// kernel/net/ip.h
//
// IPv4: parseo, validación, dispatch por protocolo, output con ruta
// trivial. Sin fragmentación, sin forwarding, sin opciones IP.

#ifndef KERNEL_NET_IP_H
#define KERNEL_NET_IP_H

#include "skb.h"
#include <stddef.h>
#include <stdint.h>

#define IP_HDR_MIN_LEN 20

// Protocolos de transporte (IANA).
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

// Bits del campo frag_off (RFC 791), en HOST order.
#define IP_FLAG_DF 0x4000u
#define IP_FLAG_MF 0x2000u
#define IP_FRAG_OFFSET_MASK 0x1FFFu

#define IP_ADDR_BROADCAST 0xFFFFFFFFu // 255.255.255.255

// Handler de protocolo. El skb llega SIN header IP ni Ethernet: skb_data
// apunta al inicio del payload de transporte (ICMP/UDP/TCP header
// incluido). El handler es responsable de consumir (liberar o reusar)
// el skb.
typedef void (*ip_rx_handler_t)(skb_t *skb, uint32_t src_ip, uint32_t dst_ip,
                                uint8_t proto);

// Inicializa el módulo. Limpia la tabla de handlers y registra el
// handler de ETH_P_IP en netif_rx.
void ip_init(void);

// Registra/reemplaza el handler de un protocolo. Sin lock (llamar
// solo en init, en frío).
int ip_register_protocol(uint8_t proto, ip_rx_handler_t fn);

// Procesa un paquete IP. El skb NO lleva cabecera Ethernet.
// Consume el skb siempre (drop → libera; OK → pasa al handler).
// Devuelve 0 si se despachó, <0 si se descartó.
int ip_input(skb_t *skb);

// Envía un paquete IP. El skb contiene SOLO el payload del protocolo.
//   src_ip == 0 → autoseleccionar el IP del netif de salida.
//   dst_ip      → host order.
// En éxito el skb está consumido por la ruta. En error, ip_output lo
// libera. Devuelve 0 si OK, <0 si error (p. ej. -ENETUNREACH).
int ip_output(skb_t *skb, uint32_t src_ip, uint32_t dst_ip, uint8_t proto);

#endif