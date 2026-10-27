// kernel/cmdline.c — ver cmdline.h
#include "cmdline.h"
#include "string.h"

#define CMDLINE_MAX 256

static char g_cmdline[CMDLINE_MAX];

void cmdline_init(const char *s) {
  size_t n = 0;
  if (s)
    while (n < CMDLINE_MAX - 1 && s[n])
      n++;
  memcpy(g_cmdline, s ? s : "", n);
  g_cmdline[n] = '\0';
}

const char *kernel_cmdline(void) { return g_cmdline; }

int cmdline_find(const char *line, const char *key, char *out,
                 size_t outlen) {
  size_t kl = strlen(key);
  int found = 0;
  if (!line || !kl)
    return 0;
  for (const char *p = line; *p;) {
    while (*p == ' ' || *p == '\t')
      p++;
    const char *tok = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    size_t tl = (size_t)(p - tok);
    if (tl < kl || strncmp(tok, key, kl) != 0 ||
        (tl > kl && tok[kl] != '='))
      continue;
    found = 1;
    if (out && outlen) {
      const char *v = tl > kl ? tok + kl + 1 : tok + tl;
      size_t vl = (size_t)(tok + tl - v);
      if (vl >= outlen)
        vl = outlen - 1;
      memcpy(out, v, vl);
      out[vl] = '\0';
    }
  }
  return found;
}

int cmdline_get(const char *key, char *out, size_t outlen) {
  return cmdline_find(g_cmdline, key, out, outlen);
}
