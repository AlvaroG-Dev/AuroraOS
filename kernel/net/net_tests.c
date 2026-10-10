// kernel/net/net_tests.c
//
// Tests del stack de red (Fase 0). Solo pool, ops skb, netif y
// checksums. Sin protocolos, sin driver.

#include "../klog.h"
#include "../string.h"
#include "../test.h"
#include "byteorder.h"
#include "checksum.h"
#include "netif.h"
#include "skb.h"
#include <stdint.h>

// Ethertype privado para los tests de dispatch. No colisiona con
// ETH_P_IP (0x0800) — si usáramos ETH_P_IP, netif_register_handler()
// sobrescribiría el handler que ip_init() instala para IPv4 y los
// tests de Fase 2 nunca verían el paquete.
#define TEST_MOCK_ETHERTYPE 0x88B7u

// ---------------------------------------------------------------------------
// skb pool
// ---------------------------------------------------------------------------
static void test_skb_alloc_free(void) {
  int used_before = skb_pool_used();

  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc() devolvió NULL");
  if (!s)
    return;
  TEST_ASSERT_EQ(skb_pool_used(), used_before + 1);
  TEST_ASSERT_EQ(skb_len(s), 0);
  TEST_ASSERT_EQ(skb_headroom(s), SKB_DEFAULT_HEADROOM);

  skb_free(s);
  TEST_ASSERT_EQ(skb_pool_used(), used_before);
}
REGISTER_TEST("net: skb alloc/free", test_skb_alloc_free);

static void test_skb_exhaust_pool(void) {
  static skb_t *ptrs[SKB_POOL_SIZE];

  for (int i = 0; i < SKB_POOL_SIZE; i++) {
    ptrs[i] = skb_alloc();
    TEST_ASSERT(ptrs[i] != NULL, "skb_alloc falló en i=%d", i);
    if (!ptrs[i]) {
      for (int j = 0; j < i; j++)
        skb_free(ptrs[j]);
      return;
    }
  }

  // Pool agotado: siguiente devuelve NULL.
  skb_t *nul = skb_alloc();
  TEST_ASSERT(nul == NULL, "pool agotado pero alloc devolvió skb");

  for (int i = 0; i < SKB_POOL_SIZE; i++)
    skb_free(ptrs[i]);
}
REGISTER_TEST("net: skb pool exhaust", test_skb_exhaust_pool);

// ---------------------------------------------------------------------------
// skb operaciones
// ---------------------------------------------------------------------------
static void test_skb_push_pull(void) {
  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "alloc");
  if (!s)
    return;

  // Escribir 8 bytes en el payload.
  uint8_t *p = (uint8_t *)skb_put(s, 8);
  TEST_ASSERT(p != NULL, "put(8)");
  if (p) {
    for (int i = 0; i < 8; i++)
      p[i] = (uint8_t)(0xAA + i);
  }
  TEST_ASSERT_EQ(skb_len(s), 8);

  // Push 4 bytes de cabecera.
  uint8_t *h = (uint8_t *)skb_push(s, 4);
  TEST_ASSERT(h != NULL, "push(4)");
  if (h) {
    h[0] = 0xDE;
    h[1] = 0xAD;
    h[2] = 0xBE;
    h[3] = 0xEF;
  }
  TEST_ASSERT_EQ(skb_len(s), 12);

  // Pull 4 bytes (quitar cabecera).
  uint8_t *q = (uint8_t *)skb_pull(s, 4);
  TEST_ASSERT(q != NULL, "pull(4)");
  if (q) {
    TEST_ASSERT(q[0] == 0xAA, "payload[0] tras pull");
    TEST_ASSERT(q[7] == 0xB1, "payload[7] tras pull");
  }
  TEST_ASSERT_EQ(skb_len(s), 8);

  skb_free(s);
}
REGISTER_TEST("net: skb push/pull", test_skb_push_pull);

static void test_skb_trim(void) {
  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "alloc");
  if (!s)
    return;

  uint8_t *p = (uint8_t *)skb_put(s, 16);
  TEST_ASSERT(p != NULL, "put");
  TEST_ASSERT_EQ(skb_len(s), 16);

  skb_trim(s, 6);
  TEST_ASSERT_EQ(skb_len(s), 10);

  skb_free(s);
}
REGISTER_TEST("net: skb trim", test_skb_trim);

// ---------------------------------------------------------------------------
// byte order
// ---------------------------------------------------------------------------
static void test_byteorder(void) {
  TEST_ASSERT_EQ(bswap16(0x1234), 0x3412);
  TEST_ASSERT_EQ(bswap32(0x12345678), 0x78563412);
  TEST_ASSERT_EQ(bswap64(0x0102030405060708ULL), 0x0807060504030201ULL);

  TEST_ASSERT_EQ(htons(0x1234), 0x3412);
  TEST_ASSERT_EQ(ntohs(0x3412), 0x1234);
  TEST_ASSERT_EQ(htonl(0x12345678), 0x78563412);
  TEST_ASSERT_EQ(ntohl(0x78563412), 0x12345678);
}
REGISTER_TEST("net: byte order", test_byteorder);

// ---------------------------------------------------------------------------
// checksum
// ---------------------------------------------------------------------------
static void test_checksum_simple(void) {
  // [0x0001, 0x0002, 0x0003] → suma 0x0006, complemento 0xFFF9
  uint8_t data[6] = {0x00, 0x01, 0x00, 0x02, 0x00, 0x03};
  uint16_t c = inet_csum(data, 6);
  TEST_ASSERT_EQ(c, 0xFFF9);

  // Byte impar: [0x00, 0x01, 0x00, 0x02, 0x00]
  // → 0x0001 + 0x0002 + 0x0000 = 0x0003, complemento 0xFFFC
  uint8_t odd[5] = {0x00, 0x01, 0x00, 0x02, 0x00};
  uint16_t c2 = inet_csum(odd, 5);
  TEST_ASSERT_EQ(c2, 0xFFFC);

  // Verificar: si metemos el complemento, la verificación pasa.
  // (No hace falta montar un paquete real, basta con comprobar que
  //  la suma completa da 0.)
  uint32_t sum = inet_csum_partial(data, 6, 0);
  sum += c; // añadir el complemento
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  TEST_ASSERT_EQ((uint16_t)~sum, 0);
}
REGISTER_TEST("net: checksum simple", test_checksum_simple);

static void test_checksum_pseudo(void) {
  // Pseudo-header de un paquete UDP de 8 bytes:
  //   src = 10.0.0.1 (0x0A000001)
  //   dst = 10.0.0.2 (0x0A000002)
  //   proto = 17
  //   len = 8
  // Los 8 bytes del payload son 0.
  // El resultado debe ser determinista; solo comprobamos que dos
  // llamadas con los mismos argumentos dan el mismo resultado y que
  // el complemento verifica.
  uint8_t data[8] = {0};
  uint16_t c = inet_csum_pseudo(0x0A000001u, 0x0A000002u, 17, data, 8);

  // Meter el checksum en el offset 6 (campo UDP checksum) y verificar.
  data[6] = (uint8_t)((c >> 8) & 0xFF);
  data[7] = (uint8_t)(c & 0xFF);
  int rc = inet_csum_pseudo_verify(0x0A000001u, 0x0A000002u, 17, data, 8);
  TEST_ASSERT_EQ(rc, 0);
}
REGISTER_TEST("net: checksum pseudo-header", test_checksum_pseudo);

// ---------------------------------------------------------------------------
// netif
// ---------------------------------------------------------------------------
static uint8_t g_mock_tx_frame[SKB_BUF_SIZE];
static int g_mock_tx_count;
static int g_mock_rx_hits;

static int mock_transmit(netif_t *netif, skb_t *skb) {
  (void)netif;
  size_t len = skb_len(skb);
  if (len > sizeof(g_mock_tx_frame)) {
    skb_free(skb);
    return -1;
  }
  memcpy(g_mock_tx_frame, skb_data(skb), len);
  g_mock_tx_count++;
  skb_free(skb);
  return 0;
}

static void mock_rx_handler(netif_t *netif, skb_t *skb) {
  (void)netif;
  g_mock_rx_hits++;
  skb_free(skb);
}

static netif_t g_mock_netif;
static netif_ops_t g_mock_ops;

static void mock_netif_setup(void) {
  memset(&g_mock_netif, 0, sizeof(g_mock_netif));
  const char *nm = "mock0";
  for (int i = 0; i < 5; i++)
    g_mock_netif.name[i] = nm[i];
  g_mock_netif.name[5] = '\0';
  g_mock_netif.mac[0] = 0x02;
  g_mock_netif.mac[1] = 0x00;
  g_mock_netif.mac[2] = 0x00;
  g_mock_netif.mac[3] = 0x00;
  g_mock_netif.mac[4] = 0x00;
  g_mock_netif.mac[5] = 0x01;
  g_mock_netif.ip = 0x0A000001u; // 10.0.0.1
  g_mock_netif.netmask = 0xFFFFFF00u;
  g_mock_netif.gateway = 0;

  memset(&g_mock_ops, 0, sizeof(g_mock_ops));
  g_mock_ops.transmit = mock_transmit;
  g_mock_netif.ops = &g_mock_ops;

  netif_register(&g_mock_netif);
}

static void mock_netif_teardown(void) { netif_unregister(&g_mock_netif); }

static void test_netif_register_lookup(void) {
  mock_netif_setup();

  netif_t *n = netif_lookup("mock0");
  TEST_ASSERT(n == &g_mock_netif, "lookup por nombre");

  netif_t *by_ip = netif_lookup_by_ip(0x0A000001u);
  TEST_ASSERT(by_ip == &g_mock_netif, "lookup por IP");

  netif_t *nul = netif_lookup("nonexistent");
  TEST_ASSERT(nul == NULL, "lookup de nombre inexistente");

  mock_netif_teardown();
  netif_t *after = netif_lookup("mock0");
  TEST_ASSERT(after == NULL, "lookup tras unregister");
}
REGISTER_TEST("net: netif register/lookup", test_netif_register_lookup);

static void test_netif_tx_prepends_eth(void) {
  mock_netif_setup();
  g_mock_tx_count = 0;

  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "alloc");
  if (!s) {
    mock_netif_teardown();
    return;
  }

  s->protocol = ETH_P_IP;
  s->dst_mac[0] = 0xAA;
  s->dst_mac[1] = 0xBB;
  s->dst_mac[2] = 0xCC;
  s->dst_mac[3] = 0xDD;
  s->dst_mac[4] = 0xEE;
  s->dst_mac[5] = 0xFF;
  s->netif = &g_mock_netif;

  uint8_t *p = (uint8_t *)skb_put(s, 4);
  TEST_ASSERT(p != NULL, "put");
  if (p) {
    p[0] = 0x11;
    p[1] = 0x22;
    p[2] = 0x33;
    p[3] = 0x44;
  }

  int rc = netif_tx(&g_mock_netif, s);
  TEST_ASSERT_EQ(rc, 0);
  TEST_ASSERT_EQ(g_mock_tx_count, 1);

  // Verificar el frame Ethernet.
  TEST_ASSERT(g_mock_tx_frame[0] == 0xAA, "dst_mac[0]");
  TEST_ASSERT(g_mock_tx_frame[5] == 0xFF, "dst_mac[5]");
  TEST_ASSERT(g_mock_tx_frame[6] == 0x02, "src_mac[0]");
  TEST_ASSERT(g_mock_tx_frame[11] == 0x01, "src_mac[5]");
  TEST_ASSERT(g_mock_tx_frame[12] == 0x08, "eth type hi");
  TEST_ASSERT(g_mock_tx_frame[13] == 0x00, "eth type lo");
  TEST_ASSERT(g_mock_tx_frame[14] == 0x11, "payload[0]");
  TEST_ASSERT(g_mock_tx_frame[17] == 0x44, "payload[3]");

  mock_netif_teardown();
}
REGISTER_TEST("net: netif tx prepends Ethernet", test_netif_tx_prepends_eth);

static void test_netif_rx_dispatch(void) {
  mock_netif_setup();
  g_mock_rx_hits = 0;

  int rc = netif_register_handler(TEST_MOCK_ETHERTYPE, mock_rx_handler);
  TEST_ASSERT_EQ(rc, 0);

  // Montar un frame Ethernet con ethertype IP.
  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "alloc");
  if (!s) {
    mock_netif_teardown();
    return;
  }

  s->netif = &g_mock_netif;
  uint8_t *p = (uint8_t *)skb_put(s, 14);
  TEST_ASSERT(p != NULL, "put eth");
  if (p) {
    // dst, src cualquiera
    memset(p, 0, 12);
    p[12] = (uint8_t)(TEST_MOCK_ETHERTYPE >> 8);
    p[13] = (uint8_t)(TEST_MOCK_ETHERTYPE & 0xFF);
  }

  netif_rx(s);
  TEST_ASSERT_EQ(g_mock_rx_hits, 1);

  // Ethertype no registrado: drop silencioso.
  skb_t *s2 = skb_alloc();
  if (s2) {
    s2->netif = &g_mock_netif;
    uint8_t *p2 = (uint8_t *)skb_put(s2, 14);
    if (p2) {
      memset(p2, 0, 12);
      p2[12] = 0x12;
      p2[13] = 0x34; // no registrado
    }
    netif_rx(s2);
    TEST_ASSERT_EQ(g_mock_rx_hits, 1); // no incrementó
  }

  mock_netif_teardown();
}
REGISTER_TEST("net: netif rx dispatch", test_netif_rx_dispatch);

static void test_netif_rx_short_drop(void) {
  mock_netif_setup();
  g_mock_rx_hits = 0;
  netif_register_handler(TEST_MOCK_ETHERTYPE, mock_rx_handler);

  // Frame demasiado corto (< 14 B): debe descartarse sin crash.
  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "alloc");
  if (s) {
    s->netif = &g_mock_netif;
    skb_put(s, 8);
    netif_rx(s);
    TEST_ASSERT_EQ(g_mock_rx_hits, 0);
  }

  mock_netif_teardown();
}
REGISTER_TEST("net: netif rx drops short frames", test_netif_rx_short_drop);