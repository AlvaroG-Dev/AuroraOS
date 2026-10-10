// kernel/net/ip_tests.c
//
// Tests de Fase 2: IPv4.

#include "../klog.h"
#include "../string.h"
#include "../test.h"
#include "checksum.h"
#include "ip.h"
#include "loopback.h"
#include "netif.h"
#include "skb.h"
#include <stdint.h>

// Protocolo "experimental" (IANA 253/254) para tests. No choca con
// ICMP/TCP/UDP ni con los ethertypes de Fase 0/1.
#define IP_PROTO_TEST 253

#define IP_LO 0x7F000001u  // 127.0.0.1
#define IP_A 0x0A000001u   // 10.0.0.1 (source ficticio en input)
#define IP_EXT 0x08080808u // 8.8.8.8 (no local, sin ruta)

// ---------------------------------------------------------------------------
// Handler de test
// ---------------------------------------------------------------------------
static int g_hits;
static uint32_t g_last_src;
static uint32_t g_last_dst;
static uint8_t g_last_proto;
static uint8_t g_last_payload[128];
static size_t g_last_payload_len;

static void test_handler(skb_t *skb, uint32_t src, uint32_t dst,
                         uint8_t proto) {
  g_hits++;
  g_last_src = src;
  g_last_dst = dst;
  g_last_proto = proto;
  size_t len = skb_len(skb);
  if (len > sizeof(g_last_payload))
    len = sizeof(g_last_payload);
  memcpy(g_last_payload, skb_data(skb), len);
  g_last_payload_len = len;
  skb_free(skb);
}

static void reset_state(void) {
  g_hits = 0;
  g_last_src = 0;
  g_last_dst = 0;
  g_last_proto = 0;
  g_last_payload_len = 0;
  memset(g_last_payload, 0, sizeof(g_last_payload));
}

// ---------------------------------------------------------------------------
// Helper: skb con Ethernet + IP + payload listo para netif_rx.
//   corrupt_csum    → altera 1 bit del checksum IP.
//   override_total_len (≠0) → fuerza un total_len distinto en wire.
// ---------------------------------------------------------------------------
static skb_t *build_ip_skb(uint32_t src, uint32_t dst, uint8_t proto,
                           const void *payload, size_t payload_len,
                           int corrupt_csum, uint16_t override_total_len) {
  skb_t *s = skb_alloc();
  if (!s)
    return NULL;

  // Ethernet dummy.
  uint8_t *eth = (uint8_t *)skb_put(s, 14);
  if (!eth) {
    skb_free(s);
    return NULL;
  }
  memset(eth, 0, 12);
  eth[12] = 0x08;
  eth[13] = 0x00;

  uint8_t *ip = (uint8_t *)skb_put(s, IP_HDR_MIN_LEN);
  if (!ip) {
    skb_free(s);
    return NULL;
  }
  memset(ip, 0, IP_HDR_MIN_LEN);

  uint16_t total_len = (uint16_t)(IP_HDR_MIN_LEN + payload_len);
  if (override_total_len != 0)
    total_len = override_total_len;

  ip[0] = 0x45;
  ip[2] = (uint8_t)(total_len >> 8);
  ip[3] = (uint8_t)(total_len & 0xFF);
  ip[4] = 0;
  ip[5] = 1; // id
  ip[6] = 0;
  ip[7] = 0;  // frag
  ip[8] = 64; // TTL
  ip[9] = proto;
  ip[10] = 0;
  ip[11] = 0; // csum temporal
  ip[12] = (uint8_t)(src >> 24);
  ip[13] = (uint8_t)(src >> 16);
  ip[14] = (uint8_t)(src >> 8);
  ip[15] = (uint8_t)(src & 0xFF);
  ip[16] = (uint8_t)(dst >> 24);
  ip[17] = (uint8_t)(dst >> 16);
  ip[18] = (uint8_t)(dst >> 8);
  ip[19] = (uint8_t)(dst & 0xFF);

  if (payload_len) {
    uint8_t *p = (uint8_t *)skb_put(s, payload_len);
    if (!p) {
      skb_free(s);
      return NULL;
    }
    memcpy(p, payload, payload_len);
  }

  uint16_t csum = inet_csum(ip, IP_HDR_MIN_LEN);
  if (corrupt_csum)
    csum ^= 0x0001;
  ip[10] = (uint8_t)(csum >> 8);
  ip[11] = (uint8_t)(csum & 0xFF);

  return s;
}

// Recalcula el checksum del header IP in-place tras una mutación de prueba.
static void fix_ip_csum(skb_t *s) {
  uint8_t *ip = skb_data(s) + 14;
  ip[10] = 0;
  ip[11] = 0;
  uint16_t csum = inet_csum(ip, IP_HDR_MIN_LEN);
  ip[10] = (uint8_t)(csum >> 8);
  ip[11] = (uint8_t)(csum & 0xFF);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void test_ip_input_ok(void) {
  ip_register_protocol(IP_PROTO_TEST, test_handler);
  reset_state();

  const char *payload = "hello-ip";
  skb_t *s = build_ip_skb(IP_A, IP_LO, IP_PROTO_TEST, payload, 8, 0, 0);
  TEST_ASSERT(s != NULL, "build falló");
  if (!s)
    return;

  s->netif = loopback_netif();
  netif_rx(s);

  TEST_ASSERT_EQ(g_hits, 1);
  TEST_ASSERT_EQ(g_last_src, IP_A);
  TEST_ASSERT_EQ(g_last_dst, IP_LO);
  TEST_ASSERT_EQ(g_last_proto, IP_PROTO_TEST);
  TEST_ASSERT_EQ(g_last_payload_len, 8u);
  TEST_ASSERT(memcmp(g_last_payload, payload, 8) == 0, "payload mal");
}
REGISTER_TEST("net/ip: input básico OK", test_ip_input_ok);

static void test_ip_input_bad_version(void) {
  ip_register_protocol(IP_PROTO_TEST, test_handler);
  reset_state();

  skb_t *s = build_ip_skb(IP_A, IP_LO, IP_PROTO_TEST, "x", 1, 0, 0);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;
  skb_data(s)[14] = 0x65; // version=6, ihl=5
  fix_ip_csum(s);

  s->netif = loopback_netif();
  netif_rx(s);
  TEST_ASSERT_EQ(g_hits, 0);
}
REGISTER_TEST("net/ip: versión != 4 dropped", test_ip_input_bad_version);

static void test_ip_input_bad_ihl(void) {
  ip_register_protocol(IP_PROTO_TEST, test_handler);
  reset_state();

  skb_t *s = build_ip_skb(IP_A, IP_LO, IP_PROTO_TEST, "x", 1, 0, 0);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;
  skb_data(s)[14] = 0x44; // version=4, ihl=4 (<5)
  fix_ip_csum(s);

  s->netif = loopback_netif();
  netif_rx(s);
  TEST_ASSERT_EQ(g_hits, 0);
}
REGISTER_TEST("net/ip: IHL < 5 dropped", test_ip_input_bad_ihl);

static void test_ip_input_bad_checksum(void) {
  ip_register_protocol(IP_PROTO_TEST, test_handler);
  reset_state();

  skb_t *s = build_ip_skb(IP_A, IP_LO, IP_PROTO_TEST, "x", 1, 1, 0);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;

  s->netif = loopback_netif();
  netif_rx(s);
  TEST_ASSERT_EQ(g_hits, 0);
}
REGISTER_TEST("net/ip: checksum malo dropped", test_ip_input_bad_checksum);

static void test_ip_input_dst_not_local(void) {
  ip_register_protocol(IP_PROTO_TEST, test_handler);
  reset_state();

  skb_t *s = build_ip_skb(IP_A, IP_EXT, IP_PROTO_TEST, "x", 1, 0, 0);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;

  s->netif = loopback_netif();
  netif_rx(s);
  TEST_ASSERT_EQ(g_hits, 0);
}
REGISTER_TEST("net/ip: dst no local dropped", test_ip_input_dst_not_local);

static void test_ip_input_no_proto_handler(void) {
  reset_state();

  // Aseguramos que 253 tiene handler, pero no el proto 200.
  ip_register_protocol(IP_PROTO_TEST, test_handler);

  skb_t *s = build_ip_skb(IP_A, IP_LO, 200, "x", 1, 0, 0);
  TEST_ASSERT(s != NULL, "build");
  if (!s)
    return;

  s->netif = loopback_netif();
  netif_rx(s);
  TEST_ASSERT_EQ(g_hits, 0);
}
REGISTER_TEST("net/ip: proto sin handler dropped",
              test_ip_input_no_proto_handler);

static void test_ip_output_loopback(void) {
  ip_register_protocol(IP_PROTO_TEST, test_handler);
  reset_state();

  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc");
  if (!s)
    return;

  const char *payload = "outbound!";
  uint8_t *p = (uint8_t *)skb_put(s, 9);
  TEST_ASSERT(p != NULL, "skb_put");
  if (!p) {
    skb_free(s);
    return;
  }
  memcpy(p, payload, 9);

  int rc = ip_output(s, 0, IP_LO, IP_PROTO_TEST);
  TEST_ASSERT_EQ(rc, 0);
  TEST_ASSERT_EQ(g_hits, 1);
  TEST_ASSERT_EQ(g_last_src, IP_LO); // autoseleccionado desde 'lo'
  TEST_ASSERT_EQ(g_last_dst, IP_LO);
  TEST_ASSERT_EQ(g_last_proto, IP_PROTO_TEST);
  TEST_ASSERT_EQ(g_last_payload_len, 9u);
  TEST_ASSERT(memcmp(g_last_payload, payload, 9) == 0, "payload mal");
}
REGISTER_TEST("net/ip: output loopback OK", test_ip_output_loopback);

static void test_ip_output_no_route(void) {
  reset_state();

  // En el sistema real (QEMU Q35) eth0 tiene gateway 10.0.2.2, así que
  // CUALQUIER destino es enrutable vía default gw. Para probar el caso
  // "sin ruta" apagamos temporalmente el gateway.
  netif_t *eth0 = netif_lookup("eth0");
  uint32_t saved_gw = 0;
  if (eth0) {
    saved_gw = eth0->gateway;
    eth0->gateway = 0;
  }

  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc");
  if (s) {
    skb_put(s, 4);
    // 8.8.8.8 no está ni en 127/8 (lo) ni en 10.0.2.0/24 (eth0).
    // Sin gateway, no hay ruta → -ENETUNREACH.
    int rc = ip_output(s, 0, IP_EXT, IP_PROTO_TEST);
    TEST_ASSERT(rc < 0, "ip_output aceptó sin ruta: rc=%d", rc);
  }

  if (eth0)
    eth0->gateway = saved_gw;
}
REGISTER_TEST("net/ip: output sin ruta → error", test_ip_output_no_route);