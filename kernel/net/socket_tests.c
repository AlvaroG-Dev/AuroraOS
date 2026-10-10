// kernel/net/socket_tests.c

#include "../klog.h"
#include "../string.h"
#include "../test.h"
#include "byteorder.h"
#include "checksum.h"
#include "ip.h"
#include "loopback.h"
#include "netif.h"
#include "skb.h"
#include "socket.h"
#include "udp.h"
#include <stdint.h>

#define IP_LOOP 0x7F000001u
#define PORT_CLIENT 0xC000u
#define PORT_SERVER 0xC001u
#define IP_EXT 0x0A000001u

// Construye un frame Ethernet+IP+UDP para inyectar.
static skb_t *build_udp_skb(uint32_t src, uint32_t dst, uint16_t sport,
                            uint16_t dport, const void *payload,
                            size_t payload_len) {
  skb_t *s = skb_alloc();
  if (!s)
    return NULL;

  uint8_t *eth = skb_put(s, 14);
  if (!eth) {
    skb_free(s);
    return NULL;
  }
  memset(eth, 0, 12);
  eth[12] = 0x08;
  eth[13] = 0x00;

  size_t udp_len = 8 + payload_len;

  uint8_t *ip = skb_put(s, 20);
  if (!ip) {
    skb_free(s);
    return NULL;
  }
  memset(ip, 0, 20);
  uint16_t total = (uint16_t)(20 + udp_len);
  ip[0] = 0x45;
  ip[2] = (uint8_t)(total >> 8);
  ip[3] = (uint8_t)(total & 0xFF);
  ip[8] = 64;
  ip[9] = 17;
  ip[12] = (uint8_t)(src >> 24);
  ip[13] = (uint8_t)(src >> 16);
  ip[14] = (uint8_t)(src >> 8);
  ip[15] = (uint8_t)(src & 0xFF);
  ip[16] = (uint8_t)(dst >> 24);
  ip[17] = (uint8_t)(dst >> 16);
  ip[18] = (uint8_t)(dst >> 8);
  ip[19] = (uint8_t)(dst & 0xFF);
  uint16_t ipc = inet_csum(ip, 20);
  ip[10] = (uint8_t)(ipc >> 8);
  ip[11] = (uint8_t)(ipc & 0xFF);

  uint8_t *u = skb_put(s, udp_len);
  if (!u) {
    skb_free(s);
    return NULL;
  }
  u[0] = (uint8_t)(sport >> 8);
  u[1] = (uint8_t)(sport & 0xFF);
  u[2] = (uint8_t)(dport >> 8);
  u[3] = (uint8_t)(dport & 0xFF);
  u[4] = (uint8_t)(udp_len >> 8);
  u[5] = (uint8_t)(udp_len & 0xFF);
  u[6] = 0;
  u[7] = 0;
  if (payload_len)
    memcpy(u + 8, payload, payload_len);
  uint16_t uc = inet_csum_pseudo(src, dst, IP_PROTO_UDP, u, udp_len);
  if (uc == 0)
    uc = 0xFFFF;
  u[6] = (uint8_t)(uc >> 8);
  u[7] = (uint8_t)(uc & 0xFF);

  return s;
}

// ---------------------------------------------------------------------------
// 1. Create + bind + send/recv loopback.
// ---------------------------------------------------------------------------
static void test_socket_create_bind(void) {
  socket_t *s = socket_create(SOCK_DGRAM, SOCK_PROTO_AUTO);
  TEST_ASSERT(s != NULL, "socket_create");
  if (!s)
    return;

  int rc = socket_bind(s, SOCK_ADDR_ANY, PORT_CLIENT);
  TEST_ASSERT_EQ(rc, 0);
  TEST_ASSERT_EQ(socket_local_port(s), PORT_CLIENT);
  TEST_ASSERT(socket_local_ip(s) != 0, "IP local autodetectada");

  // Segundo bind al mismo puerto debe fallar.
  socket_t *s2 = socket_create(SOCK_DGRAM, SOCK_PROTO_AUTO);
  TEST_ASSERT(s2 != NULL, "second create");
  if (s2) {
    rc = socket_bind(s2, SOCK_ADDR_ANY, PORT_CLIENT);
    TEST_ASSERT(rc < 0, "doble bind aceptado: rc=%d", rc);
    socket_put(s2);
  }

  socket_put(s);
}
REGISTER_TEST("net/sock: create + bind + EADDRINUSE", test_socket_create_bind);

// ---------------------------------------------------------------------------
// 2. Envío desde socket y recepción por otro socket (vía loopback).
// ---------------------------------------------------------------------------
static void test_socket_sendto_recvfrom(void) {
  socket_t *srv = socket_create(SOCK_DGRAM, SOCK_PROTO_AUTO);
  socket_t *cli = socket_create(SOCK_DGRAM, SOCK_PROTO_AUTO);
  TEST_ASSERT(srv && cli, "create");
  if (!srv || !cli) {
    if (srv)
      socket_put(srv);
    if (cli)
      socket_put(cli);
    return;
  }

  TEST_ASSERT_EQ(socket_bind(srv, SOCK_ADDR_ANY, PORT_SERVER), 0);
  TEST_ASSERT_EQ(socket_bind(cli, SOCK_ADDR_ANY, PORT_CLIENT), 0);

  const char *msg = "hola-socket";
  int rc = socket_sendto(cli, msg, 11, IP_LOOP, PORT_SERVER);
  TEST_ASSERT_EQ(rc, 0);

  // El servidor debe tener el paquete encolado.
  char buf[64] = {0};
  uint32_t sip = 0;
  uint16_t sp = 0;
  int wb = 0;
  int n = socket_recvfrom(srv, buf, sizeof(buf), &sip, &sp, &wb);
  TEST_ASSERT(n == 11, "recvfrom devolvió %d", n);
  TEST_ASSERT(memcmp(buf, msg, 11) == 0, "payload mal");
  TEST_ASSERT_EQ(sp, PORT_CLIENT);
  TEST_ASSERT_EQ(sip, IP_LOOP);

  socket_put(cli);
  socket_put(srv);
}
REGISTER_TEST("net/sock: sendto + recvfrom por loopback",
              test_socket_sendto_recvfrom);

// ---------------------------------------------------------------------------
// 3. Non-blocking recv sobre cola vacía → -EAGAIN.
// ---------------------------------------------------------------------------
static void test_socket_nonblock_recv(void) {
  socket_t *s = socket_create(SOCK_DGRAM, SOCK_PROTO_AUTO);
  TEST_ASSERT(s != NULL, "create");
  if (!s)
    return;

  TEST_ASSERT_EQ(socket_bind(s, SOCK_ADDR_ANY, 0xC002u), 0);
  socket_set_nonblock(s, 1);

  char buf[16];
  int wb = 0;
  int n = socket_recvfrom(s, buf, sizeof(buf), NULL, NULL, &wb);
  TEST_ASSERT(n < 0, "recvfrom debería fallar con -EAGAIN, fue %d", n);
  TEST_ASSERT(wb == 1, "would_block no marcado");

  socket_put(s);
}
REGISTER_TEST("net/sock: recvfrom non-block EAGAIN", test_socket_nonblock_recv);

// ---------------------------------------------------------------------------
// 4. Dispatch desde input externo (simula NIC).
// ---------------------------------------------------------------------------
static void test_socket_input_dispatch(void) {
  socket_t *s = socket_create(SOCK_DGRAM, SOCK_PROTO_AUTO);
  TEST_ASSERT(s != NULL, "create");
  if (!s)
    return;
  TEST_ASSERT_EQ(socket_bind(s, SOCK_ADDR_ANY, 0xC003u), 0);

  const char *payload = "rx-extern";
  skb_t *sk = build_udp_skb(IP_EXT, IP_LOOP, 0xBEEF, 0xC003u, payload, 9);
  TEST_ASSERT(sk != NULL, "build");
  if (!sk) {
    socket_put(s);
    return;
  }
  sk->netif = loopback_netif();
  netif_rx(sk);

  char buf[32] = {0};
  uint32_t sip = 0;
  uint16_t sp = 0;
  int wb = 0;
  int n = socket_recvfrom(s, buf, sizeof(buf), &sip, &sp, &wb);
  TEST_ASSERT(n == 9, "recvfrom devolvió %d", n);
  TEST_ASSERT(memcmp(buf, payload, 9) == 0, "payload mal");
  TEST_ASSERT_EQ(sip, IP_EXT);
  TEST_ASSERT_EQ(sp, 0xBEEFu);

  socket_put(s);
}
REGISTER_TEST("net/sock: dispatch desde NIC simulada",
              test_socket_input_dispatch);