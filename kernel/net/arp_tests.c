// kernel/net/arp_tests.c
//
// Tests de Fase 3: ARP.

#include "../klog.h"
#include "../string.h"
#include "../test.h"
#include "../time.h"
#include "../uaccess.h"
#include "arp.h"
#include "byteorder.h"
#include "ip.h"
#include "netif.h"
#include "skb.h"
#include <stdint.h>

#define IP_TEST 0x0A000001u // 10.0.0.1
#define IP_PEER 0x0A000002u // 10.0.0.2

// ---------------------------------------------------------------------------
// netif mock que captura los frames transmitidos.
// ---------------------------------------------------------------------------
static netif_t g_nif;
static netif_ops_t g_ops;

#define TX_LOG_MAX 16
static uint8_t g_tx_frames[TX_LOG_MAX][128];
static size_t g_tx_lens[TX_LOG_MAX];
static int g_tx_count;

static int mock_tx(netif_t *netif, skb_t *skb) {
  (void)netif;
  size_t len = skb_len(skb);
  if (g_tx_count < TX_LOG_MAX) {
    if (len > sizeof(g_tx_frames[0]))
      len = sizeof(g_tx_frames[0]);
    memcpy(g_tx_frames[g_tx_count], skb_data(skb), len);
    g_tx_lens[g_tx_count] = len;
    g_tx_count++;
  }
  skb_free(skb);
  return 0;
}

static void mock_setup(void) {
  arp_flush(); // <-- NUEVO: parte de tabla limpia
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
  g_nif.netmask = 0xFFFFFF00u; // /24
  g_nif.gateway = 0;
  memset(&g_ops, 0, sizeof(g_ops));
  g_ops.transmit = mock_tx;
  g_nif.ops = &g_ops;
  netif_register(&g_nif);

  g_tx_count = 0;
}

static void mock_teardown(void) { netif_unregister(&g_nif); }

static void tx_log_reset(void) { g_tx_count = 0; }

// Arma un ARP request/reply y lo entrega a netif_rx.
//   op = 1 (req) / 2 (reply).
// El "sender" es `sip`/`smac`; el "target" es `tip`/`tmac`.
static void inject_arp(netif_t *n, uint16_t op, const uint8_t smac[6],
                       uint32_t sip, const uint8_t tmac[6], uint32_t tip) {
  skb_t *s = skb_alloc();
  if (!s)
    return;

  uint8_t *eth = (uint8_t *)skb_put(s, 14);
  if (!eth) {
    skb_free(s);
    return;
  }
  memset(eth, 0, 12);
  eth[12] = 0x08;
  eth[13] = 0x06;

  uint8_t *a = (uint8_t *)skb_put(s, 28);
  if (!a) {
    skb_free(s);
    return;
  }
  memset(a, 0, 28);
  a[0] = 0x00;
  a[1] = 0x01; // htype=1
  a[2] = 0x08;
  a[3] = 0x00; // ptype=0x0800
  a[4] = 6;
  a[5] = 4;
  a[6] = (uint8_t)(op >> 8);
  a[7] = (uint8_t)(op & 0xFF);
  memcpy(a + 8, smac, 6);
  a[14] = (uint8_t)(sip >> 24);
  a[15] = (uint8_t)(sip >> 16);
  a[16] = (uint8_t)(sip >> 8);
  a[17] = (uint8_t)(sip & 0xFF);
  memcpy(a + 18, tmac, 6);
  a[24] = (uint8_t)(tip >> 24);
  a[25] = (uint8_t)(tip >> 16);
  a[26] = (uint8_t)(tip >> 8);
  a[27] = (uint8_t)(tip & 0xFF);

  s->netif = n;
  netif_rx(s);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void test_arp_resolve_creates_incomplete(void) {
  mock_setup();
  tx_log_reset();

  uint8_t mac[6];
  int rc = arp_resolve(&g_nif, IP_PEER, mac);
  TEST_ASSERT_EQ(rc, -EAGAIN);
  // Debe haber mandado un request.
  TEST_ASSERT_EQ(g_tx_count, 1);

  // El frame capturado debe ser Ethernet broadcast + ARP request.
  TEST_ASSERT_EQ(g_tx_lens[0], (size_t)(14 + 28));
  TEST_ASSERT(g_tx_frames[0][0] == 0xFF && g_tx_frames[0][5] == 0xFF,
              "dst_mac no es broadcast");
  TEST_ASSERT(g_tx_frames[0][12] == 0x08 && g_tx_frames[0][13] == 0x06,
              "ethertype != ARP");
  TEST_ASSERT(g_tx_frames[0][20] == 0x00 && g_tx_frames[0][21] == 0x01,
              "op != request");

  mock_teardown();
}
REGISTER_TEST("net/arp: resolve crea INCOMPLETE y manda request",
              test_arp_resolve_creates_incomplete);

static void test_arp_reply_learns(void) {
  mock_setup();
  tx_log_reset();

  const uint8_t peer_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

  // Inyectamos un reply directo (sin haber mandado request antes).
  inject_arp(&g_nif, 2, peer_mac, IP_PEER, g_nif.mac, IP_TEST);

  // Ahora resolve debe devolver 0 con la MAC aprendida.
  uint8_t mac[6] = {0};
  int rc = arp_resolve(&g_nif, IP_PEER, mac);
  TEST_ASSERT_EQ(rc, 0);
  TEST_ASSERT(memcmp(mac, peer_mac, 6) == 0, "MAC aprendida incorrecta");

  mock_teardown();
}
REGISTER_TEST("net/arp: reply aprende MAC", test_arp_reply_learns);

static void test_arp_request_answered(void) {
  mock_setup();
  tx_log_reset();

  const uint8_t requester_mac[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};

  // El peer nos pregunta quién tiene IP_TEST.
  inject_arp(&g_nif, 1, requester_mac, IP_PEER, (const uint8_t[6]){0}, IP_TEST);

  // Debemos haber emitido exactamente un reply.
  TEST_ASSERT_EQ(g_tx_count, 1);
  TEST_ASSERT_EQ(g_tx_lens[0], (size_t)(14 + 28));
  // Ethernet dst = MAC del requester.
  TEST_ASSERT(memcmp(g_tx_frames[0] + 0, requester_mac, 6) == 0,
              "reply dst_mac != requester");
  // op = reply.
  TEST_ASSERT(g_tx_frames[0][20] == 0x00 && g_tx_frames[0][21] == 0x02,
              "op != reply");
  // Sender IP = nuestra IP.
  TEST_ASSERT(g_tx_frames[0][28] == 0x0A && g_tx_frames[0][31] == 0x01,
              "sender IP mal");

  mock_teardown();
}
REGISTER_TEST("net/arp: request a nuestra IP genera reply",
              test_arp_request_answered);

static void test_arp_pending_queue(void) {
  mock_setup();
  tx_log_reset();

  // 1) No hay entry: ip_output a IP_PEER debe quedar encolado.
  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc");
  if (!s) {
    mock_teardown();
    return;
  }
  uint8_t *p = (uint8_t *)skb_put(s, 4);
  if (p) {
    p[0] = 0xAA;
    p[1] = 0xBB;
    p[2] = 0xCC;
    p[3] = 0xDD;
  }

  int rc = ip_output(s, 0, IP_PEER, 253 /* proto test */);
  TEST_ASSERT_EQ(rc, 0);
  // Tras ip_output: 1 ARP request en la cola de TX del mock.
  TEST_ASSERT_EQ(g_tx_count, 1);
  TEST_ASSERT(g_tx_frames[0][12] == 0x08 && g_tx_frames[0][13] == 0x06,
              "esperaba ARP request, no otro ethertype");

  // 2) Llega el reply con la MAC del peer.
  const uint8_t peer_mac[6] = {0x02, 0xAA, 0x11, 0x22, 0x33, 0x44};
  inject_arp(&g_nif, 2, peer_mac, IP_PEER, g_nif.mac, IP_TEST);

  // 3) Debe haberse desencolado y transmitido el IP pendiente.
  TEST_ASSERT_EQ(g_tx_count, 2);
  // Frame #1 = el IP pendiente.
  TEST_ASSERT(g_tx_frames[1][12] == 0x08 && g_tx_frames[1][13] == 0x00,
              "frame pendiente no es IPv4");
  TEST_ASSERT(memcmp(g_tx_frames[1] + 0, peer_mac, 6) == 0,
              "frame pendiente no va al peer");

  mock_teardown();
}
REGISTER_TEST("net/arp: cola de pending se vacía con el reply",
              test_arp_pending_queue);

static void test_arp_aging(void) {
  mock_setup();
  tx_log_reset();

  const uint8_t peer_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  inject_arp(&g_nif, 2, peer_mac, IP_PEER, g_nif.mac, IP_TEST);

  uint8_t mac[6] = {0};
  TEST_ASSERT_EQ(arp_resolve(&g_nif, IP_PEER, mac), 0);

  // Avanzamos el reloj del kernel artificialmente.
  uint64_t saved = tick_count;
  tick_count = saved + ARP_REACHABLE_TTL_MS + 1;
  arp_timer_tick();

  // Sigue siendo válida la MAC (STALE también es usable).
  memset(mac, 0, 6);
  TEST_ASSERT_EQ(arp_resolve(&g_nif, IP_PEER, mac), 0);
  TEST_ASSERT(memcmp(mac, peer_mac, 6) == 0, "STALE perdió la MAC");

  // Avanzamos más: debe expirar.
  tick_count = saved + ARP_REACHABLE_TTL_MS + ARP_STALE_TTL_MS + 1;
  arp_timer_tick();

  // Ahora resolve debe volver a armar INCOMPLETE (nueva request).
  tx_log_reset();
  TEST_ASSERT_EQ(arp_resolve(&g_nif, IP_PEER, mac), -EAGAIN);
  TEST_ASSERT_EQ(g_tx_count, 1);

  tick_count = saved;

  mock_teardown();
}
REGISTER_TEST("net/arp: aging expira entradas", test_arp_aging);