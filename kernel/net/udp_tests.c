// kernel/net/udp_tests.c

#include "../klog.h"
#include "../string.h"
#include "../test.h"
#include "byteorder.h"
#include "checksum.h"
#include "ip.h"
#include "loopback.h"
#include "netif.h"
#include "skb.h"
#include "udp.h"
#include <stdint.h>

#define IP_LOOP 0x7F000001u
#define UDP_PORT_TEST 0x1234u
#define UDP_PORT_OTHER 0x5678u
#define IP_EXT 0x0A000001u

static int g_hits;
static uint32_t g_src_ip, g_dst_ip;
static uint16_t g_src_port, g_dst_port;
static uint8_t g_payload[128];
static size_t g_payload_len;

static void test_handler(skb_t *skb, uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port) {
  g_hits++;
  g_src_ip = src_ip;
  g_dst_ip = dst_ip;
  g_src_port = src_port;
  g_dst_port = dst_port;
  size_t len = skb_len(skb);
  if (len > sizeof(g_payload))
    len = sizeof(g_payload);
  memcpy(g_payload, skb_data(skb), len);
  g_payload_len = len;
  skb_free(skb);
}

static void reset_state(void) {
  g_hits = 0;
  g_src_ip = g_dst_ip = 0;
  g_src_port = g_dst_port = 0;
  g_payload_len = 0;
  memset(g_payload, 0, sizeof(g_payload));
}

// Construye un frame completo Ethernet + IP + UDP + payload.
// corrupt_csum: si != 0, XOR con 1 en el campo checksum UDP.
static skb_t *build_udp_skb(uint32_t src, uint32_t dst, uint16_t sport,
                            uint16_t dport, const void *payload,
                            size_t payload_len, int corrupt_csum) {
  skb_t *s = skb_alloc();
  if (!s)
    return NULL;

  uint8_t *eth = (uint8_t *)skb_put(s, 14);
  if (!eth) {
    skb_free(s);
    return NULL;
  }
  memset(eth, 0, 12);
  eth[12] = 0x08;
  eth[13] = 0x00;

  size_t udp_len = 8 + payload_len;

  uint8_t *ip = (uint8_t *)skb_put(s, 20);
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

  uint8_t *u = (uint8_t *)skb_put(s, udp_len);
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
  if (corrupt_csum)
    uc ^= 0x0001;
  u[6] = (uint8_t)(uc >> 8);
  u[7] = (uint8_t)(uc & 0xFF);

  return s;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. Roundtrip vía loopback: udp_output + handler registrado en el mismo
// puerto.
static void test_udp_output_roundtrip(void) {
  udp_register_port(UDP_PORT_TEST, test_handler);
  reset_state();

  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc");
  if (!s)
    return;

  const char *payload = "hello-udp";
  uint8_t *p = (uint8_t *)skb_put(s, 9);
  TEST_ASSERT(p != NULL, "skb_put");
  if (!p) {
    skb_free(s);
    return;
  }
  memcpy(p, payload, 9);

  int rc = udp_output(s, IP_LOOP, IP_LOOP, UDP_PORT_OTHER, UDP_PORT_TEST);
  TEST_ASSERT_EQ(rc, 0);
  TEST_ASSERT_EQ(g_hits, 1);
  TEST_ASSERT_EQ(g_src_ip, IP_LOOP);
  TEST_ASSERT_EQ(g_dst_ip, IP_LOOP);
  TEST_ASSERT_EQ(g_src_port, UDP_PORT_OTHER);
  TEST_ASSERT_EQ(g_dst_port, UDP_PORT_TEST);
  TEST_ASSERT_EQ(g_payload_len, 9u);
  TEST_ASSERT(memcmp(g_payload, payload, 9) == 0, "payload mal");
}
REGISTER_TEST("net/udp: output roundtrip por loopback",
              test_udp_output_roundtrip);

// 2. Input directo: inyectamos un frame UDP válido por netif_rx.
static void test_udp_input_ok(void) {
  udp_register_port(UDP_PORT_TEST, test_handler);
  reset_state();

  const char *payload = "ping-pong";
  skb_t *s = build_udp_skb(IP_EXT, IP_LOOP, UDP_PORT_OTHER, UDP_PORT_TEST,
                           payload, 9, /*corrupt=*/0);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;
  s->netif = loopback_netif();
  netif_rx(s);

  TEST_ASSERT_EQ(g_hits, 1);
  TEST_ASSERT_EQ(g_src_ip, IP_EXT);
  TEST_ASSERT_EQ(g_dst_ip, IP_LOOP);
  TEST_ASSERT_EQ(g_src_port, UDP_PORT_OTHER);
  TEST_ASSERT_EQ(g_dst_port, UDP_PORT_TEST);
  TEST_ASSERT_EQ(g_payload_len, 9u);
  TEST_ASSERT(memcmp(g_payload, payload, 9) == 0, "payload mal");
}
REGISTER_TEST("net/udp: input OK", test_udp_input_ok);

// 3. Checksum malo → drop silencioso.
static void test_udp_bad_csum(void) {
  udp_register_port(UDP_PORT_TEST, test_handler);
  reset_state();

  skb_t *s = build_udp_skb(IP_EXT, IP_LOOP, UDP_PORT_OTHER, UDP_PORT_TEST, "x",
                           1, /*corrupt=*/1);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;
  s->netif = loopback_netif();
  netif_rx(s);

  TEST_ASSERT_EQ(g_hits, 0);
}
REGISTER_TEST("net/udp: checksum malo dropped", test_udp_bad_csum);

// 4. Puerto sin handler → drop.
static void test_udp_no_handler(void) {
  reset_state();

  skb_t *s = build_udp_skb(IP_EXT, IP_LOOP, UDP_PORT_OTHER, 0x9ABCu, "x", 1,
                           /*corrupt=*/0);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;
  s->netif = loopback_netif();
  netif_rx(s);

  TEST_ASSERT_EQ(g_hits, 0);
}
REGISTER_TEST("net/udp: puerto sin handler dropped", test_udp_no_handler);