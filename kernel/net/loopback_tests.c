// kernel/net/loopback_tests.c
//
// Tests de Fase 1: loopback.

#include "../klog.h"
#include "../string.h"
#include "../test.h"
#include "byteorder.h"
#include "loopback.h"
#include "netif.h"
#include "skb.h"
#include <stdint.h>

// Ethertypes reservados para uso local/experimental. No chocan con
// 0x0800 (IP), 0x0806 (ARP), 0x86DD (IPv6), etc.
#define LO_TEST_PROTO_A 0x88B5u
#define LO_TEST_PROTO_B 0x88B6u

static int g_rx_hits;
static netif_t *g_rx_netif;
static uint16_t g_rx_proto;
static uint8_t g_rx_payload[64];
static size_t g_rx_payload_len;

static void lo_test_rx(netif_t *netif, skb_t *skb) {
  g_rx_hits++;
  g_rx_netif = netif;
  g_rx_proto = skb->protocol;

  size_t len = skb_len(skb);
  if (len > 14) {
    size_t payload = len - 14;
    if (payload > sizeof(g_rx_payload))
      payload = sizeof(g_rx_payload);
    memcpy(g_rx_payload, skb_data(skb) + 14, payload);
    g_rx_payload_len = payload;
  } else {
    g_rx_payload_len = 0;
  }
  skb_free(skb);
}

static void reset_rx_state(void) {
  g_rx_hits = 0;
  g_rx_netif = NULL;
  g_rx_proto = 0;
  g_rx_payload_len = 0;
  memset(g_rx_payload, 0, sizeof(g_rx_payload));
}

// ---------------------------------------------------------------------------
// lo registrado y localizable
// ---------------------------------------------------------------------------
static void test_lo_registered(void) {
  netif_t *lo = loopback_netif();
  TEST_ASSERT(lo != NULL, "loopback_netif() devolvió NULL");
  if (!lo)
    return;

  TEST_ASSERT(netif_lookup("lo") == lo, "lookup por nombre");
  TEST_ASSERT(netif_lookup_by_ip(0x7F000001u) == lo, "lookup por IP");

  TEST_ASSERT_EQ(lo->ip, 0x7F000001u);
  TEST_ASSERT_EQ(lo->netmask, 0xFF000000u);
  TEST_ASSERT_EQ(lo->gateway, 0u);
}
REGISTER_TEST("net/lo: registrado", test_lo_registered);

// ---------------------------------------------------------------------------
// tx → rx: un paquete sale y vuelve.
// ---------------------------------------------------------------------------
static void test_lo_tx_roundtrip(void) {
  netif_t *lo = loopback_netif();
  TEST_ASSERT(lo != NULL, "lo NULL");
  if (!lo)
    return;

  int rc = netif_register_handler(LO_TEST_PROTO_A, lo_test_rx);
  TEST_ASSERT_EQ(rc, 0);

  reset_rx_state();

  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc");
  if (!s)
    return;

  s->protocol = LO_TEST_PROTO_A;
  // dst_mac irrelevante en lo, pero netif_tx la copia.
  s->dst_mac[0] = 0x11;
  s->dst_mac[1] = 0x22;
  s->dst_mac[2] = 0x33;
  s->dst_mac[3] = 0x44;
  s->dst_mac[4] = 0x55;
  s->dst_mac[5] = 0x66;

  uint8_t *p = (uint8_t *)skb_put(s, 5);
  TEST_ASSERT(p != NULL, "skb_put");
  if (p) {
    p[0] = 0xDE;
    p[1] = 0xAD;
    p[2] = 0xBE;
    p[3] = 0xEF;
    p[4] = 0x42;
  }

  uint64_t tx_before = lo->tx_packets;
  uint64_t rx_before = lo->rx_packets;

  rc = netif_tx(lo, s);
  TEST_ASSERT_EQ(rc, 0);

  TEST_ASSERT_EQ(g_rx_hits, 1);
  TEST_ASSERT(g_rx_netif == lo, "rx no pasó por 'lo'");
  TEST_ASSERT_EQ(g_rx_proto, LO_TEST_PROTO_A);
  TEST_ASSERT_EQ(g_rx_payload_len, 5u);
  TEST_ASSERT(g_rx_payload[0] == 0xDE, "payload[0]");
  TEST_ASSERT(g_rx_payload[4] == 0x42, "payload[4]");

  TEST_ASSERT_EQ(lo->tx_packets, tx_before + 1);
  TEST_ASSERT_EQ(lo->rx_packets, rx_before + 1);
}
REGISTER_TEST("net/lo: tx→rx roundtrip", test_lo_tx_roundtrip);

// ---------------------------------------------------------------------------
// Ethertype sin handler: drop silencioso.
// ---------------------------------------------------------------------------
static void test_lo_drops_unknown_ethertype(void) {
  netif_t *lo = loopback_netif();
  TEST_ASSERT(lo != NULL, "lo NULL");
  if (!lo)
    return;

  reset_rx_state();
  uint64_t rx_before = lo->rx_packets;
  uint64_t drop_before = lo->rx_dropped;

  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc");
  if (!s)
    return;

  s->protocol = 0x1234; // sin handler
  skb_put(s, 4);

  int rc = netif_tx(lo, s);
  TEST_ASSERT_EQ(rc, 0);

  TEST_ASSERT_EQ(g_rx_hits, 0);
  TEST_ASSERT_EQ(lo->rx_packets, rx_before + 1);
  TEST_ASSERT_EQ(lo->rx_dropped, drop_before + 1);
}
REGISTER_TEST("net/lo: dropea ethertype sin handler",
              test_lo_drops_unknown_ethertype);

// ---------------------------------------------------------------------------
// 64 paquetes consecutivos: contadores y payloads intactos.
// ---------------------------------------------------------------------------
static void test_lo_bulk(void) {
  netif_t *lo = loopback_netif();
  TEST_ASSERT(lo != NULL, "lo NULL");
  if (!lo)
    return;

  netif_register_handler(LO_TEST_PROTO_B, lo_test_rx);
  reset_rx_state();

  uint64_t tx_before = lo->tx_packets;
  uint64_t rx_before = lo->rx_packets;

  enum { N = 64 };
  for (int i = 0; i < N; i++) {
    skb_t *s = skb_alloc();
    TEST_ASSERT(s != NULL, "skb_alloc en i=%d", i);
    if (!s)
      return;
    s->protocol = LO_TEST_PROTO_B;
    uint8_t *p = (uint8_t *)skb_put(s, 4);
    if (p) {
      p[0] = (uint8_t)i;
      p[1] = 0;
      p[2] = 0;
      p[3] = 0;
    }
    int rc = netif_tx(lo, s);
    if (rc != 0) {
      TEST_ASSERT(0, "netif_tx falló en i=%d rc=%d", i, rc);
      return;
    }
  }

  TEST_ASSERT_EQ(g_rx_hits, N);
  TEST_ASSERT_EQ(lo->tx_packets, tx_before + N);
  TEST_ASSERT_EQ(lo->rx_packets, rx_before + N);
}
REGISTER_TEST("net/lo: 64 paquetes consecutivos", test_lo_bulk);