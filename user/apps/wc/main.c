#include "../../lib/file.h"
#include "../../lib/string.h"

typedef struct {
  int lines;
  int words;
  int bytes;
} wc_stats_t;

static void count_buf(const char *buf, size_t n, wc_stats_t *st, int *in_word) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)buf[i];
    st->bytes++;
    if (c == '\n')
      st->lines++;
    int is_space = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (is_space) {
      *in_word = 0;
    } else if (!*in_word) {
      *in_word = 1;
      st->words++;
    }
  }
}

static int wc_fd(int fd, wc_stats_t *st) {
  char buf[1024];
  int in_word = 0;
  int64_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    count_buf(buf, (size_t)n, st, &in_word);
  return n < 0 ? -1 : 0;
}

static void print_stats(const wc_stats_t *st, int flag, const char *name) {
  if (flag == 'l')
    printf("%d %s\n", st->lines, name);
  else if (flag == 'w')
    printf("%d %s\n", st->words, name);
  else if (flag == 'c')
    printf("%d %s\n", st->bytes, name);
  else
    printf("%d %d %d %s\n", st->lines, st->words, st->bytes, name);
}

int main(int argc, char **argv) {
  int flag = 0;
  int argi = 1;
  while (argi < argc && argv[argi][0] == '-' && argv[argi][1]) {
    char f = argv[argi][1];
    if (f == 'l' || f == 'w' || f == 'c') {
      flag = f;
      argi++;
    } else {
      break;
    }
  }

  if (argi >= argc) {
    wc_stats_t st = {0, 0, 0};
    if (wc_fd(0, &st) < 0) {
      puts("wc: error leyendo stdin");
      return 1;
    }
    print_stats(&st, flag, "");
    return 0;
  }

  wc_stats_t total = {0, 0, 0};
  int printed = 0;
  for (int i = argi; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      printf("wc: no existe %s\n", argv[i]);
      continue;
    }
    wc_stats_t st = {0, 0, 0};
    wc_fd(fd, &st);
    close(fd);
    print_stats(&st, flag, argv[i]);
    total.lines += st.lines;
    total.words += st.words;
    total.bytes += st.bytes;
    printed++;
  }
  if (printed > 1)
    print_stats(&total, flag, "total");
  return 0;
}