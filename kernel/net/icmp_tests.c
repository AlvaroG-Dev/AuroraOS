// kernel/net/icmp_tests.c

#include "../klog.h"
#include "../string.h"
#include "../test.h"
#include "arp.h"
#include "byteorder.h"
#include "checksum.h"
#include "icmp.h"
#include "ip.h"
#include "netif.h"
#include "skb.h"
#include <stdint.h>

#define IP_TEST 0x0A000001u
#define IP_PEER 0x0A000002u
#define IP_LOOP 0x7F000001u

// ---------------------------------------------------------------------------
// Mock netif que captura los frames transmitidos.
// ---------------------------------------------------------------------------
static netif_t g_nif;
static netif_ops_t g_ops;
static uint8_t g_tx[16][160];
static size_t g_tx_len[16];
static int g_tx_count;

static int mock_tx(netif_t *n, skb_t *s) {
  (void)n;
  size_t len = skb_len(s);
  if (g_tx_count < 16) {
    if (len > sizeof(g_tx[0]))
      len = sizeof(g_tx[0]);
    memcpy(g_tx[g_tx_count], skb_data(s), len);
    g_tx_len[g_tx_count] = len;
    g_tx_count++;
  }
  skb_free(s);
  return 0;
}

static void mock_setup(void) {
  arp_flush();
  memset(&g_nif, 0, sizeof(g_nif));
  g_nif.name[0] = 't';
  g_nif.name[1] = '0';
  g_nif.name[2] = '\0';
  g_nif.mac[0] = 0x02;
  g_nif.mac[1] = 0x11;
  g_nif.mac[2] = 0x22;
  g_nif.mac[3] = 0x33;
  g_nif.mac[4] = 0x44;
  g_nif.mac[5] = 0x55;
  g_nif.ip = IP_TEST;
  g_nif.netmask = 0xFFFFFF00u;
  g_nif.gateway = 0;
  memset(&g_ops, 0, sizeof(g_ops));
  g_ops.transmit = mock_tx;
  g_nif.ops = &g_ops;
  netif_register(&g_nif);
  g_tx_count = 0;
}

static void mock_teardown(void) { netif_unregister(&g_nif); }

// Aprende la MAC del peer sin dejar basura en g_tx.
static const uint8_t PEER_MAC[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

static void learn_peer(void) {
  // Provoca la creación del entry INCOMPLETE (+ ARP request).
  uint8_t tmp[6];
  arp_resolve(&g_nif, IP_PEER, tmp);

  // Inyecta un ARP reply con la MAC del peer.
  skb_t *s = skb_alloc();
  if (!s)
    return;
  uint8_t *eth = skb_put(s, 14);
  if (!eth) {
    skb_free(s);
    return;
  }
  memset(eth, 0, 12);
  eth[12] = 0x08;
  eth[13] = 0x06;
  uint8_t *a = skb_put(s, 28);
  if (!a) {
    skb_free(s);
    return;
  }
  memset(a, 0, 28);
  a[1] = 0x01; // htype=1 (BE 0x0001)
  a[2] = 0x08;
  a[3] = 0x00; // ptype=0x0800
  a[4] = 6;
  a[5] = 4;
  a[7] = 0x02; // op=reply
  memcpy(a + 8, PEER_MAC, 6);
  a[14] = 0x0A;
  a[15] = 0x00;
  a[16] = 0x00;
  a[17] = 0x02;
  memcpy(a + 18, g_nif.mac, 6);
  a[24] = 0x0A;
  a[25] = 0x00;
  a[26] = 0x00;
  a[27] = 0x01;
  s->netif = &g_nif;
  netif_rx(s);

  g_tx_count = 0; // descartar el ARP request
}

// Construye Ethernet + IP + ICMP para inyectar en netif_rx.
static skb_t *build_icmp_skb(uint32_t src, uint32_t dst, uint8_t type,
                             uint8_t code, uint16_t ident, uint16_t seq,
                             const void *data, size_t data_len,
                             int corrupt_csum) {
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

  size_t icmp_len = ICMP_HDR_LEN + data_len;

  uint8_t *ip = skb_put(s, 20);
  if (!ip) {
    skb_free(s);
    return NULL;
  }
  memset(ip, 0, 20);
  uint16_t total = (uint16_t)(20 + icmp_len);
  ip[0] = 0x45;
  ip[2] = total >> 8;
  ip[3] = total & 0xFF;
  ip[8] = 64;
  ip[9] = 1;
  ip[12] = (uint8_t)(src >> 24);
  ip[13] = (uint8_t)(src >> 16);
  ip[14] = (uint8_t)(src >> 8);
  ip[15] = (uint8_t)(src);
  ip[16] = (uint8_t)(dst >> 24);
  ip[17] = (uint8_t)(dst >> 16);
  ip[18] = (uint8_t)(dst >> 8);
  ip[19] = (uint8_t)(dst);
  uint16_t ipc = inet_csum(ip, 20);
  ip[10] = ipc >> 8;
  ip[11] = ipc & 0xFF;

  uint8_t *ic = skb_put(s, icmp_len);
  if (!ic) {
    skb_free(s);
    return NULL;
  }

  memset(ic, 0, ICMP_HDR_LEN); // ← NUEVO: csum=0 antes de calcular

  ic[0] = type;
  ic[1] = code;
  ic[4] = ident >> 8;
  ic[5] = ident & 0xFF;
  ic[6] = seq >> 8;
  ic[7] = seq & 0xFF;
  if (data_len)
    memcpy(ic + ICMP_HDR_LEN, data, data_len);
  uint16_t icc = inet_csum(ic, icmp_len);
  if (corrupt_csum)
    icc ^= 0x0001;
  ic[2] = icc >> 8;
  ic[3] = icc & 0xFF;

  return s;
}

// ---------------------------------------------------------------------------
// Estado del callback
// ---------------------------------------------------------------------------
static int g_cb_hits;
static uint32_t g_cb_src;
static uint16_t g_cb_ident;
static uint16_t g_cb_seq;
static uint8_t g_cb_data[64];
static size_t g_cb_data_len;

static void on_reply(uint32_t src, uint16_t ident, uint16_t seq,
                     const uint8_t *data, size_t data_len) {
  g_cb_hits++;
  g_cb_src = src;
  g_cb_ident = ident;
  g_cb_seq = seq;
  if (data_len > sizeof(g_cb_data))
    data_len = sizeof(g_cb_data);
  memcpy(g_cb_data, data, data_len);
  g_cb_data_len = data_len;
}

static void reset_cb(void) {
  g_cb_hits = 0;
  g_cb_src = 0;
  g_cb_ident = 0;
  g_cb_seq = 0;
  g_cb_data_len = 0;
  memset(g_cb_data, 0, sizeof(g_cb_data));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. Echo request → se emite reply.
static void test_icmp_request_replies(void) {
  mock_setup();
  icmp_set_echo_reply_cb(NULL);
  learn_peer();

  const char *payload = "ping-payload";
  skb_t *s = build_icmp_skb(IP_PEER, IP_TEST, 8, 0, 0xBEEF, 1, payload, 12, 0);
  TEST_ASSERT(s != NULL, "build");
  if (!s) {
    mock_teardown();
    return;
  }
  s->netif = &g_nif;
  netif_rx(s);

  TEST_ASSERT_EQ(g_tx_count, 1);
  TEST_ASSERT(memcmp(g_tx[0], PEER_MAC, 6) == 0, "reply dst != peer");
  TEST_ASSERT(g_tx[0][12] == 0x08 && g_tx[0][13] == 0x00, "ethertype");
  TEST_ASSERT(g_tx[0][14 + 9] == 1, "proto != ICMP");
  TEST_ASSERT(g_tx[0][14 + 20] == 0, "type != echo reply");
  TEST_ASSERT(g_tx[0][14 + 20 + 4] == 0xBE && g_tx[0][14 + 20 + 5] == 0xEF,
              "ident");
  TEST_ASSERT(g_tx[0][14 + 20 + 6] == 0x00 && g_tx[0][14 + 20 + 7] == 0x01,
              "seq");
  TEST_ASSERT(memcmp(g_tx[0] + 14 + 20 + 8, payload, 12) == 0, "payload");

  mock_teardown();
}
REGISTER_TEST("net/icmp: echo request genera reply", test_icmp_request_replies);

// 2. Echo reply → callback.
static void test_icmp_reply_fires_cb(void) {
  mock_setup();
  reset_cb();
  icmp_set_echo_reply_cb(on_reply);

  const uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  skb_t *s = build_icmp_skb(IP_PEER, IP_TEST, 0, 0, 0x1234, 0xABCD, data,
                            sizeof(data), 0);
  TEST_ASSERT(s != NULL, "build");
  if (!s) {
    icmp_set_echo_reply_cb(NULL);
    mock_teardown();
    return;
  }
  s->netif = &g_nif;
  netif_rx(s);

  TEST_ASSERT_EQ(g_cb_hits, 1);
  TEST_ASSERT_EQ(g_cb_src, IP_PEER);
  TEST_ASSERT_EQ(g_cb_ident, 0x1234);
  TEST_ASSERT_EQ(g_cb_seq, 0xABCD);
  TEST_ASSERT_EQ(g_cb_data_len, sizeof(data));
  TEST_ASSERT(memcmp(g_cb_data, data, sizeof(data)) == 0, "data");
  TEST_ASSERT_EQ(g_tx_count, 0);

  icmp_set_echo_reply_cb(NULL);
  mock_teardown();
}
REGISTER_TEST("net/icmp: echo reply invoca callback", test_icmp_reply_fires_cb);

// 3. Checksum malo → drop silencioso.
static void test_icmp_bad_csum(void) {
  mock_setup();
  reset_cb();
  icmp_set_echo_reply_cb(on_reply);
  learn_peer();

  skb_t *s = build_icmp_skb(IP_PEER, IP_TEST, 8, 0, 0x42, 1, "x", 1, 1);
  TEST_ASSERT(s != NULL, "build");
  if (!s) {
    icmp_set_echo_reply_cb(NULL);
    mock_teardown();
    return;
  }
  s->netif = &g_nif;
  netif_rx(s);

  TEST_ASSERT_EQ(g_tx_count, 0);
  TEST_ASSERT_EQ(g_cb_hits, 0);

  icmp_set_echo_reply_cb(NULL);
  mock_teardown();
}
REGISTER_TEST("net/icmp: checksum malo dropped", test_icmp_bad_csum);

// 4. End-to-end: ping a 127.0.0.1 vía loopback.
static void test_icmp_ping_loopback(void) {
  reset_cb();
  icmp_set_echo_reply_cb(on_reply);

  const char *payload = "hola-lo";
  int rc = icmp_echo_request(IP_LOOP, 0xCAFE, 7, payload, 7);
  TEST_ASSERT_EQ(rc, 0);

  TEST_ASSERT_EQ(g_cb_hits, 1);
  TEST_ASSERT_EQ(g_cb_src, IP_LOOP);
  TEST_ASSERT_EQ(g_cb_ident, 0xCAFE);
  TEST_ASSERT_EQ(g_cb_seq, 7);
  TEST_ASSERT_EQ(g_cb_data_len, 7u);
  TEST_ASSERT(memcmp(g_cb_data, payload, 7) == 0, "payload");

  icmp_set_echo_reply_cb(NULL);
}
REGISTER_TEST("net/icmp: ping loopback end-to-end", test_icmp_ping_loopback);