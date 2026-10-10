// kernel/net/icmp.c

#include "icmp.h"
#include "../klog.h"
#include "../string.h"
#include "../uaccess.h"
#include "byteorder.h"
#include "checksum.h"
#include "ip.h"

#define ICMP_CB_MAX 4
static icmp_echo_reply_fn g_reply_cbs[ICMP_CB_MAX];

static inline uint16_t rd16be(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static inline void wr16be(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

// ---------------------------------------------------------------------------
// Input (skb ya tiene el IP header quitado por ip_input).
// ---------------------------------------------------------------------------
static void icmp_input(skb_t *skb, uint32_t src_ip, uint32_t dst_ip,
                       uint8_t proto) {
  (void)dst_ip;
  (void)proto;

  size_t len = skb_len(skb);
  if (len < ICMP_HDR_LEN) {
    skb_free(skb);
    return;
  }
  uint8_t *h = skb_data(skb);

  if (inet_csum_verify(h, len) != 0) {
    LOG_DEBUG("[ICMP] drop: checksum malo");
    skb_free(skb);
    return;
  }

  uint8_t type = h[0];
  uint8_t code = h[1];
  uint16_t ident = rd16be(h + 4);
  uint16_t seq = rd16be(h + 6);

  if (type == ICMP_TYPE_ECHO_REQUEST && code == 0) {
    // Mutación in-place: type=8 → type=0. Recalculamos checksum.
    h[0] = ICMP_TYPE_ECHO_REPLY;
    h[2] = 0;
    h[3] = 0;
    uint16_t csum = inet_csum(h, len);
    wr16be(h + 2, csum);

    // ip_output consume el skb siempre.
    (void)ip_output(skb, 0, src_ip, IP_PROTO_ICMP);
    LOG_DEBUG("[ICMP] echo reply -> %08x seq=%u", src_ip, seq);
    return;
  }

  if (type == ICMP_TYPE_ECHO_REPLY && code == 0) {
    const uint8_t *data = h + ICMP_HDR_LEN;
    size_t data_len = len - ICMP_HDR_LEN;
    for (int i = 0; i < ICMP_CB_MAX; i++) {
      if (g_reply_cbs[i])
        g_reply_cbs[i](src_ip, ident, seq, data, data_len);
    }
    skb_free(skb);
    return;
  }

  LOG_DEBUG("[ICMP] drop: tipo=%u code=%u", type, code);
  skb_free(skb);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
int icmp_output(skb_t *skb, uint32_t dst_ip, uint8_t type, uint8_t code,
                uint16_t ident, uint16_t seq) {
  if (!skb)
    return -EINVAL;

  // Límite: payload máximo tras IP(20) + ICMP(8).
  size_t payload_len = skb_len(skb);
  if (payload_len > 0xFFFFu - 20u - ICMP_HDR_LEN) {
    skb_free(skb);
    return -EMSGSIZE;
  }

  uint8_t *h = (uint8_t *)skb_push(skb, ICMP_HDR_LEN);
  if (!h) {
    skb_free(skb);
    return -ENOMEM;
  }
  h[0] = type;
  h[1] = code;
  h[2] = 0;
  h[3] = 0; // csum temporal
  wr16be(h + 4, ident);
  wr16be(h + 6, seq);

  uint16_t csum = inet_csum(h, skb_len(skb));
  wr16be(h + 2, csum);

  return ip_output(skb, 0, dst_ip, IP_PROTO_ICMP);
}

int icmp_echo_request(uint32_t dst_ip, uint16_t ident, uint16_t seq,
                      const void *data, size_t data_len) {
  skb_t *s = skb_alloc();
  if (!s)
    return -ENOMEM;

  if (data_len) {
    uint8_t *p = (uint8_t *)skb_put(s, data_len);
    if (!p) {
      skb_free(s);
      return -ENOMEM;
    }
    memcpy(p, data, data_len);
  }
  return icmp_output(s, dst_ip, ICMP_TYPE_ECHO_REQUEST, 0, ident, seq);
}

void icmp_remove_echo_reply_cb(icmp_echo_reply_fn fn) {
  if (!fn)
    return;
  for (int i = 0; i < ICMP_CB_MAX; i++) {
    if (g_reply_cbs[i] == fn)
      g_reply_cbs[i] = NULL;
  }
}

int icmp_set_echo_reply_cb(icmp_echo_reply_fn fn) {
  if (fn == NULL) {
    memset(g_reply_cbs, 0, sizeof(g_reply_cbs));
    return 0;
  }
  // Idempotente.
  for (int i = 0; i < ICMP_CB_MAX; i++) {
    if (g_reply_cbs[i] == fn)
      return 0;
  }
  for (int i = 0; i < ICMP_CB_MAX; i++) {
    if (!g_reply_cbs[i]) {
      g_reply_cbs[i] = fn;
      return 0;
    }
  }
  return -ENOMEM;
}

void icmp_init(void) {
  memset(g_reply_cbs, 0, sizeof(g_reply_cbs));
  int rc = ip_register_protocol(IP_PROTO_ICMP, icmp_input);
  if (rc != 0) {
    LOG_ERR("[ICMP] ip_register_protocol(1) rc=%d", rc);
    return;
  }
  LOG_INFO("[ICMP] listo (echo request/reply)");
}