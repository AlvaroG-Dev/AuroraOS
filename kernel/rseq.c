// kernel/rseq.c
#include "rseq.h"
#include "cpu.h"
#include "klog.h"
#include "string.h"
#include "uaccess.h"

#define RSEQ_FLAG_UNREGISTER 1
#define RSEQ_ORIG_SIZE 32
#define RSEQ_CPU_ID_UNINITIALIZED ((uint32_t)-1)

struct rseq_area {
  uint32_t cpu_id_start;
  uint32_t cpu_id;
  uint64_t rseq_cs;
  uint32_t flags;
  uint32_t node_id;
  uint32_t mm_cid;
  uint32_t pad;
};
struct rseq_cs {
  uint32_t version, flags;
  uint64_t start_ip, post_commit_offset, abort_ip;
};

static int write_cpu(uint64_t area, uint32_t cpu) {
  uint32_t v[2] = {cpu, cpu};
  return copy_to_user((void *)area, v, sizeof(v)) < 0 ? -EFAULT : 0;
}

int64_t k_rseq(uint64_t area, uint64_t len, uint64_t flags, uint64_t sig, uint64_t a5) {
  (void)a5;
  task_t *t = sched_current();
  if (!t)
    return -EFAULT;
  if (flags & ~(uint64_t)RSEQ_FLAG_UNREGISTER)
    return -EINVAL;
  if (flags & RSEQ_FLAG_UNREGISTER) {
    if (!t->rseq_ptr || t->rseq_ptr != area || t->rseq_len != len || t->rseq_sig != (uint32_t)sig)
      return -EINVAL;
    uint32_t v[2] = {0, RSEQ_CPU_ID_UNINITIALIZED};
    uint64_t zero = 0;
    copy_to_user((void *)area, v, sizeof(v));
    copy_to_user((void *)(area + 8), &zero, 8);
    t->rseq_ptr = 0;
    return 0;
  }
  if (t->rseq_ptr) {
    return (t->rseq_ptr == area && t->rseq_len == len && t->rseq_sig == (uint32_t)sig)
               ? -EBUSY
               : -EINVAL;
  }
  if (len < RSEQ_ORIG_SIZE || (area & 31) || !access_ok((void *)area, len))
    return -EINVAL;
  struct rseq_area init;
  memset(&init, 0, sizeof(init));
  init.cpu_id_start = (uint32_t)smp_processor_id();
  init.cpu_id = init.cpu_id_start;
  if (copy_to_user((void *)area, &init, sizeof(init)) < 0)
    return -EFAULT;
  t->rseq_ptr = area;
  t->rseq_len = (uint32_t)len;
  t->rseq_sig = (uint32_t)sig;
  t->rseq_seen = t->rseq_sw;
  return 0;
}

// Si rip esta dentro de la seccion critica descrita por rseq_cs, salta al
// abort_ip (tras comprobar la firma). Siempre deja rseq_cs a 0.
static void abort_cs(task_t *t, registers_t *regs) {
  uint64_t csp = 0;
  if (copy_from_user(&csp, (void *)(t->rseq_ptr + 8), 8) < 0 || csp == 0)
    return;
  struct rseq_cs cs;
  uint64_t zero = 0;
  if (copy_from_user(&cs, (void *)csp, sizeof(cs)) < 0 || cs.version != 0) {
    copy_to_user((void *)(t->rseq_ptr + 8), &zero, 8);
    return;
  }
  copy_to_user((void *)(t->rseq_ptr + 8), &zero, 8); // se limpia siempre
  if (regs->rip < cs.start_ip || regs->rip >= cs.start_ip + cs.post_commit_offset)
    return; // fuera de la seccion: nada que abortar
  uint32_t sg = 0;
  if (copy_from_user(&sg, (void *)(cs.abort_ip - 4), 4) < 0 || sg != t->rseq_sig) {
    LOG_WARN("[RSEQ] firma de abort_ip incorrecta (0x%x != 0x%x)", sg, t->rseq_sig);
    return; // Linux mata con SIGSEGV; aqui se deja seguir
  }
  regs->rip = cs.abort_ip;
}

void rseq_exit_to_user(task_t *t, registers_t *regs) {
  if (!t || !t->rseq_ptr)
    return;
  if (t->rseq_sw == t->rseq_seen)
    return; // no se ha conmutado: cpu_id sigue siendo valido
  t->rseq_seen = t->rseq_sw;
  write_cpu(t->rseq_ptr, (uint32_t)smp_processor_id());
  if (regs && (regs->cs & 3) == 3)
    abort_cs(t, regs);
}

void rseq_signal(task_t *t, registers_t *regs) {
  if (!t || !t->rseq_ptr || !regs)
    return;
  write_cpu(t->rseq_ptr, (uint32_t)smp_processor_id());
  abort_cs(t, regs);
}
