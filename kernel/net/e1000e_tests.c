// kernel/net/e1000e_tests.c
//
// Tests de Fase 5. Asumen que la NIC existe (QEMU Q35 con e1000e).
// Los tests que requieren HW pasan si e1000e_init() ya se llamó antes
// (lo hace main.c). Si la NIC no existe, test_skip.

#include "../klog.h"
#include "../pci.h"
#include "../string.h"
#include "../test.h"
#include "e1000e.h"
#include "netif.h"
#include "skb.h"
#include <stdint.h>

static void test_e1000e_nic_present(void) {
  // Enumerar por vendor/device, igual que el driver.
  pci_device_t d;
  uint8_t sb = 0, ss = 0, sf = 0;
  int found = 0;
  while (pci_find_next_device(0x02, 0x00, &sb, &ss, &sf, &d) == 0) {
    if (d.vendor_id == 0x8086 && d.device_id == 0x10D3) {
      found = 1;
      break;
    }
    if (++sf == 0) {
      sf = 0;
      if (++ss == 0) {
        ss = 0;
        sb++;
      }
    }
  }
  if (!found) {
    test_skip("sin 82574L en este sistema");
    return;
  }
  TEST_ASSERT(1, "82574L detectado en %02x:%02x.%u", d.bus, d.slot, d.func);
}
REGISTER_TEST("net/e1000e: NIC detectada", test_e1000e_nic_present);

static void test_e1000e_registered(void) {
  netif_t *n = e1000e_netif();
  if (!n) {
    test_skip("e1000e no inicializado (sin HW)");
    return;
  }
  TEST_ASSERT(n == netif_lookup("eth0"), "netif_lookup('eth0') != e1000e");
  TEST_ASSERT(n->ip == 0x0A00020Fu, "IP != 10.0.2.15 (0x%08x)", n->ip);
  TEST_ASSERT(n->netmask == 0xFFFFFF00u, "netmask != /24");
  TEST_ASSERT(n->gateway == 0x0A000202u, "gateway != 10.0.2.2");
}
REGISTER_TEST("net/e1000e: netif registrada", test_e1000e_registered);

static void test_e1000e_tx_smoke(void) {
  netif_t *n = e1000e_netif();
  if (!n) {
    test_skip("e1000e no inicializado");
    return;
  }

  // Un frame Ethernet mínimo (broadcast + ethertype 0x88B8).
  skb_t *s = skb_alloc();
  TEST_ASSERT(s != NULL, "skb_alloc");
  if (!s)
    return;

  s->protocol = 0x88B8u;
  memset(s->dst_mac, 0xFF, 6);
  uint8_t *p = (uint8_t *)skb_put(s, 4);
  if (p) {
    p[0] = 0xDE;
    p[1] = 0xAD;
    p[2] = 0xBE;
    p[3] = 0xEF;
  }
  s->netif = n;

  uint64_t tx_before = n->tx_packets;
  int rc = netif_tx(n, s);
  TEST_ASSERT_EQ(rc, 0);
  TEST_ASSERT_EQ(n->tx_packets, tx_before + 1);
}
REGISTER_TEST("net/e1000e: tx smoke test", test_e1000e_tx_smoke);

static void test_e1000e_poll_no_crash(void) {
  netif_t *n = e1000e_netif();
  if (!n) {
    test_skip("e1000e no inicializado");
    return;
  }
  // Llamar a poll 100 veces seguidas sin que nada llegue: no debe
  // crael kernel ni corromper el ring.
  for (int i = 0; i < 100; i++)
    e1000e_poll();
  TEST_ASSERT(1, "100 polls sin crash");
}
REGISTER_TEST("net/e1000e: poll no crashea", test_e1000e_poll_no_crash);