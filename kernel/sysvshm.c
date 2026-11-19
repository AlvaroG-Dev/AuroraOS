// kernel/sysvshm.c — Memoria compartida System V: shmget/shmat/shmdt/shmctl.
//
// La usan X11 (extensión MIT-SHM: xterm, Qt xcb, GTK/X11, SDL, Wine y casi
// cualquier cliente de Xwayland), PostgreSQL, y programas antiguos. Antes
// daban ENOSYS y esas apps iban por el camino lento o se negaban a arrancar.
//
// Cada segmento es un memfd interno (memfd_new_kernel), así que shmat es un
// mmap(MAP_SHARED) de ese memfd y hereda todo lo que ya funciona: páginas
// bajo demanda, fork (los VMA copian la referencia), exit/exec (la sueltan).
//
//   - nattch = referencias del memfd - 1 (la del propio segmento): cada VMA
//     que lo mapea tiene una.
//   - IPC_RMID suelta la referencia del segmento y libera el id: la memoria
//     vive mientras quede algún proceso con el segmento mapeado (Linux
//     igual; allí el id sigue visible en ipcs hasta entonces, aquí no).
//   - shmdt(addr) quita los VMA contiguos de ese segmento desde addr. Se
//     reconocen por el nombre del nodo ("SYSV<clave>", como en /proc/maps).
//   - Permisos: dueño/creador o root para IPC_SET/IPC_RMID; lectura y
//     escritura según el modo (bits de dueño, grupo y otros) para shmget y
//     shmat. Sin límites SHMMNI/SHMALL más allá de la tabla.
#include "heap.h"
#include "klog.h"
#include "memfd.h"
#include "mutex.h"
#include "pf.h"
#include "process.h"
#include "rtc.h"
#include "string.h"
#include "uaccess.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

#define SHM_SEGS 256
#define SHM_MAX_SIZE (1ULL << 32) // 4 GiB, el tope de un memfd

#define IPC_PRIVATE 0
#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_RMID 0
#define IPC_SET 1
#define IPC_STAT 2
#define IPC_INFO 3
#define SHM_LOCK 11
#define SHM_UNLOCK 12
#define SHM_STAT 13
#define SHM_INFO 14
#define SHM_STAT_ANY 15
#define SHM_RDONLY 010000
#define SHM_RND 020000
#define SHM_REMAP 040000
#define SHM_EXEC 0100000

// Layout de Linux x86_64 (struct ipc64_perm / shmid64_ds).
typedef struct {
  int32_t key;
  uint32_t uid, gid, cuid, cgid, mode;
  uint16_t seq, pad2;
  uint64_t unused1, unused2;
} k_ipc64_perm_t;

typedef struct {
  k_ipc64_perm_t perm;
  uint64_t segsz;
  int64_t atime, dtime, ctime;
  int32_t cpid, lpid;
  uint64_t nattch, unused4, unused5;
} k_shmid64_ds_t;

typedef struct {
  uint64_t shmmax, shmmin, shmmni, shmseg, shmall, unused[4];
} k_shminfo64_t;

typedef struct {
  int32_t used_ids;
  int32_t pad;
  uint64_t shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes;
} k_shm_info_t;

typedef struct {
  int used;
  uint16_t seq;
  int32_t key;
  uint64_t size;
  uint32_t uid, gid, cuid, cgid, mode;
  int32_t cpid, lpid;
  int64_t atime, dtime, ctime;
  file_descriptor_t *f; // memfd del segmento (una referencia nuestra)
} shm_seg_t;

static shm_seg_t g_segs[SHM_SEGS];
static mutex_t g_shm_lock = {0}; // a cero vale, como g_execve_lock

static void shm_lock(void) { mutex_lock(&g_shm_lock); }
static void shm_unlock(void) { mutex_unlock(&g_shm_lock); }

// id = seq * SHM_SEGS + índice (un id viejo no vale para un segmento nuevo).
static int seg_id(int i) { return (int)g_segs[i].seq * SHM_SEGS + i; }
static shm_seg_t *seg_by_id(int id) {
  if (id < 0)
    return NULL;
  int i = id % SHM_SEGS;
  shm_seg_t *s = &g_segs[i];
  if (!s->used || s->seq != (uint16_t)(id / SHM_SEGS))
    return NULL;
  return s;
}

static uint64_t seg_nattch(const shm_seg_t *s) {
  int rc = __atomic_load_n(&s->f->ref_count, __ATOMIC_ACQUIRE);
  return rc > 1 ? (uint64_t)(rc - 1) : 0;
}

static int is_owner(const process_t *p, const shm_seg_t *s) {
  return p->euid == 0 || p->euid == s->uid || p->euid == s->cuid;
}

// want: 4 = leer, 2 = escribir.
static int may_access(const process_t *p, const shm_seg_t *s, int want) {
  if (p->euid == 0)
    return 1;
  uint32_t bits;
  if (p->euid == s->uid || p->euid == s->cuid)
    bits = (s->mode >> 6) & 7;
  else if (p->egid == s->gid || p->egid == s->cgid)
    bits = (s->mode >> 3) & 7;
  else
    bits = s->mode & 7;
  return (bits & (uint32_t)want) == (uint32_t)want;
}

int64_t k_shmget(uint64_t key_u, uint64_t size, uint64_t flg, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  int32_t key = (int32_t)key_u;
  shm_lock();
  int found = -1;
  if (key != IPC_PRIVATE)
    for (int i = 0; i < SHM_SEGS; i++)
      if (g_segs[i].used && g_segs[i].key == key) {
        found = i;
        break;
      }
  if (found >= 0) {
    shm_seg_t *s = &g_segs[found];
    int64_t rc;
    if ((flg & IPC_CREAT) && (flg & IPC_EXCL))
      rc = -EEXIST;
    else if (size > s->size)
      rc = -EINVAL;
    else if (!may_access(p, s, (flg & 0400) ? 4 : 0) || !may_access(p, s, (flg & 0200) ? 2 : 0))
      rc = -EACCES;
    else
      rc = seg_id(found);
    shm_unlock();
    return rc;
  }
  if (key != IPC_PRIVATE && !(flg & IPC_CREAT)) {
    shm_unlock();
    return -ENOENT;
  }
  if (size == 0 || size > SHM_MAX_SIZE) {
    shm_unlock();
    return -EINVAL;
  }
  int slot = -1;
  for (int i = 0; i < SHM_SEGS; i++)
    if (!g_segs[i].used) {
      slot = i;
      break;
    }
  if (slot < 0) {
    shm_unlock();
    return -ENOSPC;
  }
  char name[24];
  static const char hx[] = "0123456789abcdef";
  memcpy(name, "SYSV", 4);
  for (int k = 0; k < 8; k++)
    name[4 + k] = hx[((uint32_t)key >> (28 - 4 * k)) & 0xF];
  name[12] = 0;
  file_descriptor_t *f = memfd_new_kernel(name, size);
  if (!f) {
    shm_unlock();
    return -ENOMEM;
  }
  shm_seg_t *s = &g_segs[slot];
  uint16_t seq = (uint16_t)(s->seq + 1);
  memset(s, 0, sizeof(*s));
  s->used = 1;
  s->seq = seq;
  s->key = key;
  s->size = size;
  s->uid = s->cuid = p->euid;
  s->gid = s->cgid = p->egid;
  s->mode = (uint32_t)(flg & 0777);
  s->cpid = (int32_t)p->pid;
  s->ctime = rtc_get_epoch();
  s->f = f;
  int64_t id = seg_id(slot);
  shm_unlock();
  return id;
}

int64_t k_shmat(uint64_t id, uint64_t addr, uint64_t flg, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  if (addr & 0xFFF) {
    if (!(flg & SHM_RND))
      return -EINVAL;
    addr &= ~0xFFFULL; // SHMLBA = página
  }
  shm_lock();
  shm_seg_t *s = seg_by_id((int)id);
  if (!s) {
    shm_unlock();
    return -EINVAL;
  }
  int rdonly = (flg & SHM_RDONLY) != 0;
  if (!may_access(p, s, rdonly ? 4 : 6)) {
    shm_unlock();
    return -EACCES;
  }
  file_descriptor_t *f = s->f;
  __atomic_fetch_add(&f->ref_count, 1, __ATOMIC_ACQ_REL); // la del fd temporal
  uint64_t size = s->size;
  shm_unlock();

  // mmap(MAP_SHARED) del memfd a través de un fd temporal (el VMA toma su
  // propia referencia, que es la que cuenta como "attach").
  int fd = vfs_fd_install_ref(p, f, 1);
  if (fd < 0) {
    vfs_fd_put_ref(f);
    return fd;
  }
  uint64_t prot = 1 /* READ */ | (rdonly ? 0 : 2 /* WRITE */) | ((flg & SHM_EXEC) ? 4 : 0);
  uint64_t mflags = 0x01 /* MAP_SHARED */ | (addr ? 0x10 /* MAP_FIXED */ : 0);
  if (addr && !(flg & SHM_REMAP)) {
    // Sin SHM_REMAP, Linux no pisa mapeos que ya estén ahí.
    for (uint64_t a = addr; a < addr + size; a += 4096)
      if (vma_find(p, a)) {
        vfs_close_for_proc(p, fd);
        return -EINVAL;
      }
  }
  int64_t r = sys_mmap(p, addr, size, prot, mflags, fd, 0);
  vfs_close_for_proc(p, fd);
  if (r >= 0 && (uint64_t)r < 0xFFFFFFFFFFFFF000ULL) {
    shm_lock();
    shm_seg_t *s2 = seg_by_id((int)id);
    if (s2) {
      s2->atime = rtc_get_epoch();
      s2->lpid = (int32_t)p->pid;
    }
    shm_unlock();
  }
  return r;
}

static int is_sysv_fd(const file_descriptor_t *f) {
  return f && f->node && memfd_is_node(f->node) && !memcmp(f->node->name, "SYSV", 4);
}

int64_t k_shmdt(uint64_t addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  if (addr & 0xFFF)
    return -EINVAL;
  vma_t *v = vma_find(p, addr);
  if (!v || v->start != addr || !is_sysv_fd(v->file_fd))
    return -EINVAL;
  file_descriptor_t *f = v->file_fd;
  uint64_t end = v->end;
  // El segmento puede estar en varios VMA contiguos (mprotect los parte).
  for (vma_t *n = vma_find(p, end); n && n->start == end && n->file_fd == f;
       n = vma_find(p, end))
    end = n->end;
  int64_t r = sys_munmap(p, addr, end - addr);
  if (r < 0)
    return r;
  // dtime/lpid del segmento (si sigue con id).
  shm_lock();
  for (int i = 0; i < SHM_SEGS; i++)
    if (g_segs[i].used && g_segs[i].f == f) {
      g_segs[i].dtime = rtc_get_epoch();
      g_segs[i].lpid = (int32_t)p->pid;
    }
  shm_unlock();
  return 0;
}

static void fill_ds(const shm_seg_t *s, k_shmid64_ds_t *d) {
  memset(d, 0, sizeof(*d));
  d->perm.key = s->key;
  d->perm.uid = s->uid;
  d->perm.gid = s->gid;
  d->perm.cuid = s->cuid;
  d->perm.cgid = s->cgid;
  d->perm.mode = s->mode;
  d->perm.seq = s->seq;
  d->segsz = s->size;
  d->atime = s->atime;
  d->dtime = s->dtime;
  d->ctime = s->ctime;
  d->cpid = s->cpid;
  d->lpid = s->lpid;
  d->nattch = seg_nattch(s);
}

int64_t k_shmctl(uint64_t id, uint64_t cmd, uint64_t buf, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  process_t *p = process_current();
  if (!p)
    return -EFAULT;
  cmd &= ~0x100ULL; // IPC_64
  if (cmd == IPC_INFO) {
    k_shminfo64_t in = {SHM_MAX_SIZE, 1, SHM_SEGS, SHM_SEGS, SHM_MAX_SIZE / 4096, {0}};
    if (!buf || copy_to_user((void *)buf, &in, sizeof(in)) < 0)
      return -EFAULT;
    int hi = 0;
    shm_lock();
    for (int i = 0; i < SHM_SEGS; i++)
      if (g_segs[i].used)
        hi = i;
    shm_unlock();
    return hi;
  }
  if (cmd == SHM_INFO) {
    k_shm_info_t in;
    memset(&in, 0, sizeof(in));
    int hi = 0;
    shm_lock();
    for (int i = 0; i < SHM_SEGS; i++)
      if (g_segs[i].used) {
        in.used_ids++;
        in.shm_tot += (g_segs[i].size + 4095) / 4096;
        hi = i;
      }
    shm_unlock();
    if (!buf || copy_to_user((void *)buf, &in, sizeof(in)) < 0)
      return -EFAULT;
    return hi;
  }
  shm_lock();
  shm_seg_t *s;
  if (cmd == SHM_STAT || cmd == SHM_STAT_ANY) { // id = índice de la tabla
    s = ((int64_t)id >= 0 && id < SHM_SEGS && g_segs[id].used) ? &g_segs[id] : NULL;
  } else {
    s = seg_by_id((int)id);
  }
  if (!s) {
    shm_unlock();
    return -EINVAL;
  }
  int64_t rc = 0;
  switch (cmd) {
  case IPC_STAT:
  case SHM_STAT:
  case SHM_STAT_ANY: {
    if (cmd != SHM_STAT_ANY && !may_access(p, s, 4)) {
      rc = -EACCES;
      break;
    }
    k_shmid64_ds_t d;
    fill_ds(s, &d);
    if (!buf || copy_to_user((void *)buf, &d, sizeof(d)) < 0)
      rc = -EFAULT;
    else if (cmd != IPC_STAT)
      rc = seg_id((int)(s - g_segs));
    break;
  }
  case IPC_SET: {
    k_shmid64_ds_t d;
    if (!is_owner(p, s)) {
      rc = -EPERM;
      break;
    }
    if (!buf || copy_from_user(&d, (void *)buf, sizeof(d)) < 0) {
      rc = -EFAULT;
      break;
    }
    s->uid = d.perm.uid;
    s->gid = d.perm.gid;
    s->mode = (s->mode & ~0777u) | (d.perm.mode & 0777u);
    s->ctime = rtc_get_epoch();
    break;
  }
  case IPC_RMID: {
    if (!is_owner(p, s)) {
      rc = -EPERM;
      break;
    }
    file_descriptor_t *f = s->f;
    s->used = 0;
    s->f = NULL;
    shm_unlock();
    vfs_fd_put_ref(f); // la memoria sigue mientras algún proceso la tenga mapeada
    return 0;
  }
  case SHM_LOCK:
  case SHM_UNLOCK:
    rc = is_owner(p, s) ? 0 : -EPERM; // sin swap de estas páginas
    break;
  default:
    rc = -EINVAL;
  }
  shm_unlock();
  return rc;
}
