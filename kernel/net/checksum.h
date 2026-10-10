// kernel/net/checksum.h
//
// Internet checksum (RFC 1071). Todo el stack lo usa:
//   - IP header (sobre los 20+ bytes de la cabecera IP)
//   - UDP / TCP (sobre pseudo-header + cabecera + payload)
//
// La suma es de complemento a 1 sobre palabras de 16 bits en
// network order. El resultado es el complemento a 1 de la suma.

#ifndef KERNEL_NET_CHECKSUM_H
#define KERNEL_NET_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

// Suma parcial. `initial` permite acumular sobre múltiples buffers
// (útil para pseudo-header + payload). Devuelve la suma SIN
// complementar (el llamante hace ~ al final).
uint32_t inet_csum_partial(const void *data, size_t len, uint32_t initial);

// Checksum final sobre un solo buffer. Equivale a
// ~inet_csum_partial(data, len, 0).
uint16_t inet_csum(const void *data, size_t len);

// Checksum con pseudo-header IPv4. Protocol: 6 (TCP) o 17 (UDP).
// saddr/daddr en HOST ORDER. El buffer `data` incluye la cabecera
// de transporte + payload (sin pseudo-header).
uint16_t inet_csum_pseudo(uint32_t saddr, uint32_t daddr, uint8_t proto,
                          const void *data, size_t len);

// Verificación: devuelve 0 si OK (el checksum del paquete es
// correcto), -1 si mal. Asume que el checksum field está en el
// buffer (offset 10 para IPv4 header, offset 6 para UDP/TCP).
int inet_csum_verify(const void *data, size_t len);
int inet_csum_pseudo_verify(uint32_t saddr, uint32_t daddr, uint8_t proto,
                            const void *data, size_t len);

#endif