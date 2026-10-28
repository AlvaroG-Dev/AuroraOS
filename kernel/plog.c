// kernel/plog.c
#include "plog.h"
#include "../bootloader/include/persist_log.h"
#include "klog.h"
#include "paging.h"
#include "string.h"

static struct plog_header *g_cur;
static struct plog_header *g_prev;

void plog_attach(uint64_t base, uint64_t size) {
  if (!base || size < 2 * PLOG_SLOT_SIZE)
    return;
  struct plog_header *cur = (struct plog_header *)phys_to_virt(base);
  struct plog_header *prev =
      (struct plog_header *)phys_to_virt(base + PLOG_SLOT_SIZE);
  if (!plog_valid(cur))
    return; // el bootloader no la dejo inicializada

  if (plog_valid(prev) && plog_len(prev) > 0)
    g_prev = prev;
  LOG_INFO("[PLOG] region %p (boot #%lu), log anterior: %lu bytes",
           (void *)base, (unsigned long)cur->boot_seq,
           (unsigned long)(g_prev ? plog_len(g_prev) : 0));

  // Volcar lo que klog ya tiene (todo lo anterior a este punto).
  static char tmp[16384]; // >= KLOG_RING_SIZE
  size_t n = klog_peek(tmp, sizeof(tmp));
  if (n > klog_size())
    n = klog_size();
  g_cur = cur;
  for (size_t i = 0; i < n; i++)
    plog_putc(tmp[i]);
}

void plog_putc(char c) {
  struct plog_header *h = g_cur;
  if (!h || c == '\r')
    return;
  plog_data(h)[h->head] = (uint8_t)c;
  uint32_t nh = h->head + 1;
  h->head = nh >= h->capacity ? 0 : nh;
  h->total++;
}

size_t plog_prev_len(void) { return g_prev ? plog_len(g_prev) : 0; }

size_t plog_prev_read(char *out, size_t max) {
  if (!g_prev)
    return 0;
  return plog_linear(g_prev, (uint8_t *)out, max > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                               : (uint32_t)max);
}
