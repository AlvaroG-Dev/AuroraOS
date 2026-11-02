// Casos POSIX de sistema de ficheros: una linea por caso con el resultado o
// el errno. Se ejecuta sobre tmpfs y sobre ext2 y se comparan las salidas.
//   ext2_posix <dir>
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *en(int e) {
  switch (e) {
  case 0: return "ok"; case EPERM: return "EPERM"; case ENOENT: return "ENOENT";
  case EEXIST: return "EEXIST"; case ENOTDIR: return "ENOTDIR"; case EISDIR: return "EISDIR";
  case EINVAL: return "EINVAL"; case ENOTEMPTY: return "ENOTEMPTY"; case ELOOP: return "ELOOP";
  case ENAMETOOLONG: return "ENAMETOOLONG"; case EXDEV: return "EXDEV"; case EACCES: return "EACCES";
  case EMLINK: return "EMLINK"; case EROFS: return "EROFS"; case EBUSY: return "EBUSY";
  default: { static char b[16]; snprintf(b, sizeof b, "errno%d", e); return b; }
  }
}
#define R(name, expr) do { errno = 0; long _v = (long)(expr); int _e = errno; \
  printf("%s: %s%s\n", name, (_v < 0 || _e) && _v < 0 ? en(_e) : "ok", ""); } while (0)
static void put(const char *p, const char *d) {
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) { if (write(fd, d, strlen(d)) < 0) {} close(fd); }
}
static void get(const char *p, char *buf, size_t n) {
  int fd = open(p, O_RDONLY); ssize_t k = fd >= 0 ? read(fd, buf, n - 1) : -1;
  buf[k > 0 ? k : 0] = 0; if (fd >= 0) close(fd);
}
static long nl(const char *p) { struct stat s; return stat(p, &s) ? -1 : (long)s.st_nlink; }
static int ex(const char *p) { struct stat s; return lstat(p, &s) == 0; }
#define V(name, fmt, ...) printf("%s: " fmt "\n", name, __VA_ARGS__)


// --mmap <dir>: MAP_SHARED escribible sobre el fichero. Imprime OK/FALLO por paso.
static int mm_fail;
#define CHK(name, cond) do { int _c = (cond); printf("mmap %s: %s\n", name, _c ? "OK" : "FALLO"); if (!_c) mm_fail = 1; } while (0)
static int run_mmap(const char *dir) {
  mkdir(dir, 0755);
  char path[300]; snprintf(path, sizeof path, "%s/m1", dir);
  const size_t SZ = 3 * 4096 + 100;
  unsigned char *ref = malloc(SZ);
  for (size_t i = 0; i < SZ; i++) ref[i] = (unsigned char)(i * 7 + 3);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHK("crear", fd >= 0 && write(fd, ref, SZ) == (ssize_t)SZ);
  unsigned char *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHK("mmap rw shared", m != MAP_FAILED);
  if (m == MAP_FAILED) return 1;
  CHK("lectura inicial", memcmp(m, ref, SZ) == 0);
  m[10] = 0xAA; ref[10] = 0xAA; m[4096 + 2000] = 0xBB; ref[4096 + 2000] = 0xBB; m[SZ - 1] = 0xCC; ref[SZ - 1] = 0xCC;
  unsigned char *chk = malloc(SZ);
  CHK("read() ve la escritura por mmap", pread(fd, chk, SZ, 0) == (ssize_t)SZ && memcmp(chk, ref, SZ) == 0);
  memset(chk, 0x5A, 300); if (pwrite(fd, chk, 300, 5000) != 300) {} memset(ref + 5000, 0x5A, 300);
  CHK("mmap ve write()", memcmp(m, ref, SZ) == 0);
  CHK("msync", msync(m, SZ, MS_SYNC) == 0);
  // fork: el hijo escribe en la pagina compartida
  pid_t pid = fork();
  if (pid == 0) { m[100] = 0xDD; _exit(0); }
  int st; waitpid(pid, &st, 0); ref[100] = 0xDD;
  CHK("fork comparte la pagina", m[100] == 0xDD);
  // truncar hacia abajo y volver a crecer: la cola debe quedar a cero
  CHK("ftruncate 5000", ftruncate(fd, 5000) == 0);
  CHK("ftruncate 3*4096+100", ftruncate(fd, SZ) == 0);
  memset(ref + 5000, 0, SZ - 5000);
  CHK("cola a cero tras truncar", memcmp(m, ref, SZ) == 0);
  m[SZ - 2] = 0xEE; ref[SZ - 2] = 0xEE;
  CHK("munmap", munmap(m, SZ) == 0);
  CHK("read tras munmap", pread(fd, chk, SZ, 0) == (ssize_t)SZ && memcmp(chk, ref, SZ) == 0);
  close(fd);
  // hueco: fichero disperso, escribir una pagina del medio
  snprintf(path, sizeof path, "%s/m2", dir);
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHK("ftruncate sparse", ftruncate(fd, 10 * 4096) == 0);
  unsigned char *h = mmap(NULL, 10 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHK("mmap sparse", h != MAP_FAILED);
  CHK("hueco lee ceros", h[3 * 4096] == 0 && h[9 * 4096 + 5] == 0);
  memset(h + 4 * 4096, 0x77, 4096); h[9 * 4096 + 4095] = 0x42;
  // sin msync ni munmap: el cierre/flush debe bastar
  close(fd);
  fd = open(path, O_RDONLY);
  unsigned char b4 = 0, b9 = 0; if (pread(fd, &b4, 1, 4 * 4096 + 17) < 0) {} if (pread(fd, &b9, 1, 9 * 4096 + 4095) < 0) {}
  CHK("pagina de hueco visible por read()", b4 == 0x77 && b9 == 0x42);
  close(fd);
  // permisos: abrir en solo lectura y pedir PROT_WRITE|MAP_SHARED -> EACCES
  fd = open(path, O_RDONLY);
  errno = 0; void *bad = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHK("O_RDONLY + PROT_WRITE -> EACCES", bad == MAP_FAILED && errno == EACCES);
  void *ro = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
  CHK("O_RDONLY + PROT_READ shared ok", ro != MAP_FAILED);
  close(fd);
  // mapeo que se queda abierto al terminar (la prueba cierra y sale sin munmap)
  snprintf(path, sizeof path, "%s/m3", dir);
  fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (ftruncate(fd, 8192)) {}
  unsigned char *k = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (k != MAP_FAILED) { memset(k, 0x31, 8192); }
  printf("mmap fin (m3 queda mapeado)\n"); fflush(stdout);
  return mm_fail;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  if (!strcmp(argv[1], "--remount") && argc == 4) { // <punto> <flags MS_*>
    errno = 0; int r = mount(NULL, argv[2], NULL, MS_REMOUNT | strtoul(argv[3], 0, 0), NULL);
    printf("remount %s flags=%s: %s\n", argv[2], argv[3], r ? en(errno) : "ok"); fflush(stdout); _exit(r != 0);
  }
  if (!strcmp(argv[1], "--mmap") && argc == 3) { int r = run_mmap(argv[2]); fflush(stdout); _exit(r); }
  if (!strcmp(argv[1], "--open") && argc == 3) {
    errno = 0; int fd = open(argv[2], O_RDONLY); printf("open %s: %s\n", argv[2], fd < 0 ? en(errno) : "ok"); fflush(stdout); _exit(fd < 0);
  }
  if (!strcmp(argv[1], "--exec") && argc == 3) {
    errno = 0; execl(argv[2], argv[2], (char *)0); printf("exec %s: %s\n", argv[2], en(errno)); return 1;
  }
  mkdir(argv[1], 0755);
  if (chdir(argv[1])) return 3;
  char b[64];
  setvbuf(stdout, NULL, _IOLBF, 0);

  put("f1", "1"); put("f2", "2"); mkdir("d1", 0755); mkdir("d2", 0755); mkdir("dne", 0755); put("dne/x", "x");
  R("rename file->file", rename("f1", "f2")); get("f2", b, 64); V("  contenido", "%s f1=%d", b, ex("f1"));
  R("rename file->dir", rename("f2", "d1"));
  R("rename dir->file", rename("d1", "f2"));
  R("rename dir->nonempty dir", rename("d1", "dne"));
  R("rename dir->empty dir", rename("d1", "d2")); V("  tras", "d1=%d d2=%d", ex("d1"), ex("d2"));
  R("rename self", rename("f2", "f2"));
  R("rename missing", rename("nada", "otro"));
  R("rename dot", rename(".", "zz"));
  mkdir("p", 0755); mkdir("p/q", 0755); mkdir("p/q/r", 0755);
  R("rename dir into itself", rename("p", "p/q/r/p2"));
  R("rename dir up", rename("p/q/r", "r2")); V("  nlink", "p/q=%ld", nl("p/q"));
  { struct stat a, c; stat("r2/..", &a); stat(".", &c); V("rename dir ..", "%d", a.st_ino == c.st_ino); }
  put("h1", "h");
  R("link", link("h1", "h2")); V("  nlink", "%ld", nl("h1"));
  R("link exists", link("h1", "h2"));
  R("link dir", link("p", "plink"));
  R("unlink one link", unlink("h1")); get("h2", b, 64); V("  h2", "nlink=%ld %s", nl("h2"), b);
  { put("uo", "abierto"); int fd = open("uo", O_RDONLY); unlink("uo"); ssize_t n = read(fd, b, 20); if (n < 0) n = 0; b[n] = 0; close(fd); V("unlink open", "%s exists=%d", b, ex("uo")); }
  R("rmdir nonempty", rmdir("p"));
  R("rmdir file", rmdir("f2"));
  R("unlink dir", unlink("p"));
  R("mkdir exists", mkdir("p", 0755));
  { char n255[257], n256[258]; memset(n255, 'a', 255); n255[255] = 0; memset(n256, 'a', 256); n256[256] = 0;
    int fd = open(n255, O_WRONLY | O_CREAT, 0644); R("name 255", fd); if (fd >= 0) close(fd);
    R("name 256", open(n256, O_WRONLY | O_CREAT, 0644)); }
  R("open missing dir", open("nada/x", O_WRONLY | O_CREAT, 0644));
  R("open file as dir", open("f2/x", O_WRONLY | O_CREAT, 0644));
  mkdir("n1", 0755); mkdir("n1/s", 0755); long n1 = nl("n1"); rmdir("n1/s"); V("dir nlink", "con hijo=%ld sin=%ld", n1, nl("n1"));
  symlink("lb", "la"); symlink("la", "lb");
  R("symlink loop", open("la", O_RDONLY));
  symlink("noexiste", "ld");
  R("symlink dangling", open("ld", O_RDONLY));
  { ssize_t n = readlink("ld", b, 63); b[n > 0 ? n : 0] = 0; V("readlink", "%s", b); }
  { char lt[300]; lt[0] = '/'; memset(lt + 1, 'z', 200); lt[201] = 0; symlink(lt, "lg"); char big[300]; ssize_t n = readlink("lg", big, 299); V("symlink long", "%zd", n); }
  R("symlink exists", symlink("x", "lg"));
  { struct stat s; lstat("lg", &s); V("lstat symlink", "%d", S_ISLNK(s.st_mode) != 0); }
  { int fd = open("hole", O_RDWR | O_CREAT, 0644); if (pwrite(fd, "fin", 3, 100000) < 0) {} if (pwrite(fd, "ini", 3, 0) < 0) {}
    char a[4] = {1, 1, 1, 0}; if (pread(fd, a, 3, 50000) < 0) {} struct stat s; fstat(fd, &s);
    if (ftruncate(fd, 10)) {} char c[20]; ssize_t n1_ = pread(fd, c, 20, 0); if (ftruncate(fd, 50)) {} char d[50]; ssize_t n2 = pread(fd, d, 50, 0);
    int zeros = 1; for (int i = 10; i < 50; i++) if (d[i]) zeros = 0;
    V("holes+truncate", "hueco=%d%d%d size=%ld tras_shrink=%zd tras_grow=%zd ceros=%d ini=%c%c%c", a[0], a[1], a[2], (long)s.st_size, n1_, n2, zeros, d[0], d[1], d[2]); close(fd); }
  { put("ap", "aaa"); int f1 = open("ap", O_WRONLY | O_APPEND), f2 = open("ap", O_WRONLY | O_APPEND);
    if (write(f1, "1", 1) < 0) {} if (write(f2, "2", 1) < 0) {} if (write(f1, "3", 1) < 0) {} close(f1); close(f2); get("ap", b, 64); V("O_APPEND", "%s", b); }
  put("tr", "larga"); put("tr", "c"); get("tr", b, 64); V("O_TRUNC", "%s", b);
  R("O_EXCL", open("tr", O_CREAT | O_EXCL | O_WRONLY, 0644));
  { put("se", "0123456789"); int fd = open("se", O_RDONLY); off_t o = lseek(fd, -3, SEEK_END); ssize_t n = read(fd, b, 9); b[n > 0 ? n : 0] = 0; close(fd); V("lseek end", "%ld %s", (long)o, b); }
  { int fd = open("sy", O_WRONLY | O_CREAT | O_SYNC, 0644); ssize_t n = write(fd, "sync", 4); close(fd); get("sy", b, 64); V("O_SYNC write", "%zd %s", n, b); }
  { static unsigned char buf[1 << 20]; for (int i = 0; i < (1 << 20); i++) buf[i] = (unsigned char)i;
    int fd = open("big", O_WRONLY | O_CREAT | O_TRUNC, 0644); ssize_t w = write(fd, buf, sizeof buf); close(fd);
    static unsigned char rb[1 << 20]; fd = open("big", O_RDONLY); ssize_t r = read(fd, rb, sizeof rb); close(fd);
    V("1MiB write/read", "%zd %zd igual=%d", w, r, memcmp(buf, rb, sizeof buf) == 0); }
  { struct stat s; R("chmod", chmod("f2", 0640)); stat("f2", &s); V("  modo", "%o", s.st_mode & 07777);
    R("chown", chown("f2", 1000, 100)); stat("f2", &s); V("  dueno", "%u:%u", s.st_uid, s.st_gid);
    struct timespec ts[2] = {{1700000000, 0}, {1700000001, 0}}; R("utime", utimensat(AT_FDCWD, "f2", ts, 0)); stat("f2", &s); V("  mtime", "%ld", (long)s.st_mtime); }
  { struct stat s; mkdir("sg", 0755); if (chown("sg", 0, 50)) {} chmod("sg", 02775); put("sg/f", "x"); mkdir("sg/d", 0755);
    struct stat f, d; stat("sg/f", &f); stat("sg/d", &d); (void)s; V("setgid dir", "fgid=%u dgid=%u dsgid=%d", f.st_gid, d.st_gid, (d.st_mode & S_ISGID) != 0); }
  { R("mknod chr", mknod("cd", S_IFCHR | 0666, makedev(1, 3))); struct stat s; stat("cd", &s); V("  rdev", "%u:%u", major(s.st_rdev), minor(s.st_rdev)); }
  fflush(stdout); _exit(0);
}
