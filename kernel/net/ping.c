// kernel/net/ping.c

#include "ping.h"
#include "../klog.h"
#include "../sched.h"
#include "../spinlock.h"
#include "../string.h"
#include "../wait.h"
#include "e1000e.h"
#include "icmp.h"
#include "netif.h"

#define PING_IDENT 0x4B4Bu      // "KK"
#define PING_INTERVAL 2000      // ms entre pings
#define PING_TIMEOUT 3000       // ms de espera por reply
#define PING_TARGET 0x0A000202u // 10.0.2.2 (gateway QEMU)

static spinlock_t g_lock;
static uint16_t g_seq;
static uint64_t g_sent_ticks; // 0 = no hay ping en vuelo
static int g_inflight;
static int g_got_reply;

static bool ping_never(void *arg) {
  (void)arg;
  return false;
}

static void ping_reply_cb(uint32_t src, uint16_t ident, uint16_t seq,
                          const uint8_t *data, size_t data_len) {
  (void)data;
  (void)data_len;
  if (ident != PING_IDENT)
    return;

  unsigned long flags = spin_lock_irqsave(&g_lock);
  if (g_inflight && seq == g_seq) {
    uint64_t rtt = sched_get_ticks() - g_sent_ticks;
    g_inflight = 0;
    g_got_reply = 1;
    spin_unlock_irqrestore(&g_lock, flags);
    LOG_INFO("[PING] reply from %08x seq=%u rtt=%lu ms", src, seq,
             (unsigned long)rtt);
    return;
  }
  spin_unlock_irqrestore(&g_lock, flags);
}

static void ping_sleep_ms(uint64_t ms) {
  wait_queue_t wq;
  wait_queue_init(&wq);
  wait_event_interruptible_timeout(&wq, ping_never, NULL, ms);
}

static void ping_thread(void) {
  LOG_INFO("[PING] kthread arrancado, target=10.0.2.2 ident=0x%04x",
           PING_IDENT);

  icmp_set_echo_reply_cb(ping_reply_cb);

  // Esperar a que el driver haya levantado la netif. No debería ser
  // necesario (e1000e_init corre antes), pero es barato.
  for (int i = 0; i < 100 && !e1000e_netif(); i++)
    ping_sleep_ms(100);

  while (1) {
    // Preparar el siguiente ping.
    unsigned long flags = spin_lock_irqsave(&g_lock);
    g_seq++;
    uint16_t seq = g_seq;
    g_inflight = 1;
    g_got_reply = 0;
    g_sent_ticks = sched_get_ticks();
    spin_unlock_irqrestore(&g_lock, flags);

    uint8_t payload[16];
    for (int i = 0; i < 16; i++)
      payload[i] = (uint8_t)(seq + i);

    int rc = icmp_echo_request(PING_TARGET, PING_IDENT, seq, payload,
                               sizeof(payload));
    if (rc != 0) {
      LOG_WARN("[PING] icmp_echo_request rc=%d", rc);
      unsigned long f = spin_lock_irqsave(&g_lock);
      g_inflight = 0;
      spin_unlock_irqrestore(&g_lock, f);
      ping_sleep_ms(PING_INTERVAL);
      continue;
    }

    // Esperar reply o timeout.
    uint64_t deadline = g_sent_ticks + PING_TIMEOUT;
    while (sched_get_ticks() < deadline) {
      if (g_got_reply)
        break;
      ping_sleep_ms(50);
    }

    if (!g_got_reply) {
      unsigned long f = spin_lock_irqsave(&g_lock);
      g_inflight = 0;
      spin_unlock_irqrestore(&g_lock, f);
      LOG_WARN("[PING] timeout seq=%u", seq);
    }

    // Esperar el intervalo hasta el siguiente.
    uint64_t now = sched_get_ticks();
    uint64_t next = g_sent_ticks + PING_INTERVAL;
    if (next > now)
      ping_sleep_ms(next - now);
  }
}

void ping_init(void) {
  spin_init(&g_lock);
  g_seq = 0;
  g_inflight = 0;
  g_got_reply = 0;

  task_t *t = sched_create_task(ping_thread);
  if (!t) {
    LOG_ERR("[PING] no se pudo crear el kthread");
    return;
  }
  t->cpu_affinity = 0;
}