// user/musl/apps/aurora-fsck/main.c
//
// aurora-fsck: verifica integridad de FS Aurora. Hoy solo soporta
// FAT32 (via ASYS_FS_CHECK). Es un chequeo pasivo: reporta problemas
// pero no los repara.
//
// Uso:
//   aurora-fsck [path]
// Si path se omite, lee /proc/mounts y verifica cada FAT32 montado.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

// Definición paralela a kernel/syscall.h (ABI Aurora, 0x1000+).
#define ASYS_BASE 0x1000
#define ASYS_FS_CHECK (ASYS_BASE + 0x50)

// Copia literal del struct en kernel/fat32.h. Debe mantener el mismo
// layout byte a byte.
struct fat32_check_result {
  unsigned long long clusters_total;
  unsigned long long clusters_free;
  unsigned long long clusters_used;
  unsigned long long clusters_reachable;
  unsigned long long clusters_orphan;
  unsigned long long dirs_visited;
  unsigned long long files_visited;
  unsigned long long broken_chains;
  unsigned long long loops_detected;
  unsigned long long fsinfo_free;
  unsigned long long fsinfo_next;
  int fsinfo_matches;
  int _pad[3];
};

static int check_one(const char *mount) {
  struct fat32_check_result r;
  memset(&r, 0, sizeof(r));

  long rc = syscall(ASYS_FS_CHECK, mount, &r);
  if (rc < 0) {
    if (rc == -ENOTSUP) {
      printf("%s: FS no soportado por aurora-fsck (skip)\n", mount);
      return 0;
    }
    printf("%s: error al verificar (%ld)\n", mount, rc);
    return 1;
  }

  printf("%s: %llu clusters (%llu libres, %llu usados)\n", mount,
         r.clusters_total, r.clusters_free, r.clusters_used);
  printf("  alcanzables desde root: %llu\n", r.clusters_reachable);
  printf("  dirs=%llu files=%llu\n", r.dirs_visited, r.files_visited);

  int problems = 0;
  if (r.clusters_orphan > 0) {
    printf("  HUERFANOS: %llu clusters usados no alcanzables\n",
           r.clusters_orphan);
    problems++;
  }
  if (r.broken_chains > 0) {
    printf(
        "  CADENAS ROTAS: %llu dirents apuntan a clusters libres/invalidos\n",
        r.broken_chains);
    problems++;
  }
  if (r.loops_detected > 0) {
    printf("  LOOPS: %llu ciclos detectados en la FAT\n", r.loops_detected);
    problems++;
  }
  if (r.fsinfo_free != 0 && r.fsinfo_free != 0xFFFFFFFFuLL) {
    if (r.fsinfo_matches) {
      printf("  FSInfo: %llu libres (coincide con FAT)\n", r.fsinfo_free);
    } else {
      printf("  FSInfo: %llu libres (NO coincide con FAT=%llu)\n",
             r.fsinfo_free, r.clusters_free);
      problems++;
    }
  }

  if (problems == 0) {
    printf("  OK: sin problemas detectados\n");
    return 0;
  }
  printf("  %d problema(s) detectado(s)\n", problems);
  return 1;
}

static int scan_mounts(void) {
  FILE *f = fopen("/proc/mounts", "r");
  if (!f) {
    perror("abrir /proc/mounts");
    return 1;
  }
  char line[512];
  int any = 0, failed = 0;
  while (fgets(line, sizeof(line), f)) {
    char dev[128], mnt[256], fstype[64];
    if (sscanf(line, "%127s %255s %63s", dev, mnt, fstype) != 3)
      continue;
    if (strcmp(fstype, "fat32") != 0)
      continue;
    any = 1;
    if (check_one(mnt) != 0)
      failed++;
  }
  fclose(f);
  if (!any) {
    printf("aurora-fsck: no hay FS fat32 montados\n");
    return 0;
  }
  return failed > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc >= 2)
    return check_one(argv[1]);
  return scan_mounts();
}