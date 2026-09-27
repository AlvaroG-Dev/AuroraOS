#include "../../lib/file.h"
#include "../../lib/string.h"

#define GREP_LINE_MAX 512

static char to_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int contains_ci(const char *h, size_t hlen, const char *n, size_t nlen) {
  if (nlen == 0)
    return 1;
  if (nlen > hlen)
    return 0;
  for (size_t i = 0; i + nlen <= hlen; i++) {
    int ok = 1;
    for (size_t j = 0; j < nlen; j++) {
      if (to_lower(h[i + j]) != to_lower(n[j])) {
        ok = 0;
        break;
      }
    }
    if (ok)
      return 1;
  }
  return 0;
}

static int contains_cs(const char *h, size_t hlen, const char *n, size_t nlen) {
  if (nlen == 0)
    return 1;
  if (nlen > hlen)
    return 0;
  for (size_t i = 0; i + nlen <= hlen; i++) {
    if (memcmp(h + i, n, nlen) == 0)
      return 1;
  }
  return 0;
}

static int grep_fd(int fd, const char *pattern, int ignore_case, size_t plen) {
  char line[GREP_LINE_MAX];
  size_t linelen = 0;
  int too_long = 0;

  char buf[512];
  int64_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int64_t i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n') {
        if (!too_long) {
          int found = ignore_case ? contains_ci(line, linelen, pattern, plen)
                                  : contains_cs(line, linelen, pattern, plen);
          if (found) {
            write(1, line, linelen);
            write(1, "\n", 1);
          }
        }
        linelen = 0;
        too_long = 0;
      } else if (linelen < sizeof(line) - 1) {
        line[linelen++] = c;
      } else {
        too_long = 1;
      }
    }
  }
  // Última línea sin '\n' final.
  if (linelen > 0 && !too_long) {
    int found = ignore_case ? contains_ci(line, linelen, pattern, plen)
                            : contains_cs(line, linelen, pattern, plen);
    if (found) {
      write(1, line, linelen);
      write(1, "\n", 1);
    }
  }
  return n < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
  int ignore_case = 0;
  int argi = 1;
  while (argi < argc && argv[argi][0] == '-') {
    if (strcmp(argv[argi], "-i") == 0) {
      ignore_case = 1;
      argi++;
    } else {
      break;
    }
  }
  if (argi >= argc) {
    puts("uso: grep [-i] <pattern> [file...]");
    return 1;
  }

  const char *pattern = argv[argi++];
  size_t plen = strlen(pattern);

  if (argi >= argc) {
    if (grep_fd(0, pattern, ignore_case, plen) < 0) {
      puts("grep: error leyendo stdin");
      return 1;
    }
    return 0;
  }

  for (int i = argi; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      printf("grep: no existe %s\n", argv[i]);
      continue;
    }
    grep_fd(fd, pattern, ignore_case, plen);
    close(fd);
  }
  return 0;
}