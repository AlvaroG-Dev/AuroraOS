Bloque 1 — Cerrar job control ✅ CERRADO

1.1 TASK_STOPPED en el scheduler ✅
1.2 SIGTSTP/SIGCONT reales ✅
   Fix clave: wait_common procesa SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU y
   SIGCONT internamente sin devolver EINTR (equivalente a
   do_signal_stop de Linux). Evita que un `bg` mate un nanosleep.
1.3 waitpid con WUNTRACED/WCONTINUED ✅
1.4 NOFLSH ✅
1.5 Tests ✅
   Fix extra: signal_filter_ignored no filtra SIGCHLD (el shell lo
   necesita para reapear tras read bloqueado) ni SIGCONT.
   Fix extra: process_exit empuja SIGCHLD al padre antes del wake.

Verificado end-to-end: `sleep 100 &` + `jobs` + `fg` + `Ctrl+Z` +
`bg` + `kill %1` + `kill -9 %1` con exit codes 137/143 correctos.

---

Bloque 2 — Boot configurable y pivot

2.1 /etc/aurora.conf ✅
2.2 pivot_root / switch_root ⚠️
   `vfs_pivot_root` implementado y `SYS_PIVOT_ROOT` registrado, pero
   el modelo LiveUSB (tarfs RO en `/`, FAT32 RW en `/data`) no lo usa.
   Se mantiene como infraestructura para un futuro layout alternativo.
2.3 Layout final ⚠️ → equivalente alcanzado
   /         tarfs RO (sistema, /bin, /apps, /system)
   /data     FAT32 RW
   /dev      devfs
   /proc     procfs
   Sin /initrd. Sin bind mounts.
   El modelo del TODO (pivot_root a FAT32 como raíz) fue sustituido
   por el modelo LiveUSB, más limpio y sin necesidad de pivot_root.
2.4 aurora-fsck ✅
   Binario /apps/aurora-fsck que usa ASYS_FS_CHECK. Detecta ciclos,
   cadenas rotas, huérfanos, y compara FSInfo con la FAT.
   Validación pasiva en mount (FAT[0], FAT[1], root_cluster, firmas
   FSInfo). FSInfo se actualiza en cada sync.

---

Bloque 3 — Completitud VFS/FS ✅ CERRADO

3.1 Symlinks ✅
3.2 Hard links ✅
   vfs_ops_t.link + vfs_link + k_link/k_linkat.
   tarfs devuelve -EROFS, FAT32 -EPERM.
3.3 /proc mínimo ✅
   /proc/{uptime,version,meminfo,stat,mounts,loadavg,self}
   /proc/<pid>/{stat,status,cmdline,comm,statm}
3.4 Permisos y metadatos ✅
   3.4.a mode/uid/gid en vfs_node_t y vfs_stat_t.
   3.4.b credenciales POSIX completas + umask + groups.
   3.4.c chmod/chown/umask reales + mapa RAM FAT32.
   3.4.d vfs_check_access en open/execve/access/chdir.
   3.4.e setuid/setgid en execve.
3.5 mmap file-backed / VMA_FILE ✅
3.6 Buffer cache FAT32 ✅
   LRU de 256 sectores × 512 B, hash por LBA. Todos los accesos a
   datos pasan por el cache. La FAT y FSInfo siguen directos.
   Dirty slots se vuelcan en sync.
3.7 Tests de corrupción FAT32 ✅
   Mock block device en RAM. 6 tests nuevos (135 → 141):
   - BPB fuzz con 32 semillas
   - Detección de ciclo en FAT
   - Cluster out-of-range
   - Huérfano tras crash simulado
   - Propagación de E/S durante create
   - Reparación limpia huérfanos
   Bug encontrado y corregido: fat32_gen_short_alias se tragaba
   silenciosamente los -EIO de fat32_short_name_exists.

---

Bloque 4 — Syscalls de relleno ✅ CERRADO

4.1 statfs/fstatfs ✅
4.2 utimensat, fsync, fchmod, fchown ✅
4.3 mprotect real ✅
4.4 mknod real ✅
   rdev expuesto para char/block. devfs: null=1:3, zero=1:5,
   kmsg=1:11, tty=5:0, pts/N=136:N.
   Pendiente (opcional): mknod en devfs que cree nodos con ops
   conectadas por major/minor. Hoy solo guarda rdev.
4.5 getrlimit/setrlimit ✅
   rlimits[16] por proceso, heredados en fork, preservados en execve.
   prlimit64 real con validaciones. ulimit -a funciona.
4.6 Syscalls varias ✅
   sethostname, getrusage, times. madvise (no-op), mremap.

Cierre: busybox df, stat, ulimit completos.

---

Bloque 5 — Userland / SO

5.1 /etc/passwd, /etc/group, /etc/shadow ✅
5.2 init (PID 1) ✅
   /apps/init con fork+exec+respawn (relanza el terminal si muere
   anormalmente, sale limpio con exit 0).
5.3 Reaper de zombies sin padre ✅
   Reparent a PID 1 en process_exit + waitpid(-1) en init.
5.4 getty minimal ⏳ PENDIENTE
   Alternativa ya operativa: /apps/init lanza directamente /apps/shell,
   que abre un PTY y lanza /bin/sh. No hace falta login.

Cierre: el sistema tiene init real. Los huérfanos no se acumulan.

---

Bloque 6 — Mejoras de input/terminal

6.1 Scancodes extendidos ✅
   extended_pending para 0xE0. Flechas, F1-F12, Home/End, PgUp/PgDn,
   Del, Ins. Verificado con ash (historial, Home/End).
6.2 Copy/paste ⏳ PENDIENTE
6.3 Cursor parpadeante ⏳ PENDIENTE
6.4 Serial como consola interactiva ⏳ PENDIENTE

### Bloque 7 — futex + clone(CLONE_THREAD) ✅ CERRADO

7.1 futex (WAIT/WAKE/REQUEUE básico) ✅
   Tabla hash (uaddr, pml4) → wait queues. Solo PRIVATE.
   wake_up_one + wait_queue_add/remove_locked como primitivas.
   REQUEUE/WAKE_OP/BITSET devuelven -ENOSYS (musl no los usa en el
   fast path).

7.2 clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS|...) ✅
   - task_t gana clear_child_tid.
   - process_t gana team_size.
   - process_clone_thread: comparte process_t con el padre.
   - CLONE_PARENT_SETTID / CHILD_SETTID / CHILD_CLEARTID tratados
     como flags independientes (bug: antes se mezclaban y se escribía
     el tid en __thread_list_lock de musl, corrompiendo su mutex).
   - Hijo forzado al mismo CPU del padre (cpu_affinity) para evitar
     que otro CPU lo programe antes de que el padre termine la
     inserción en la lista de musl.

7.3 pthread_create/join funcional ✅
   - test_thread: 4 tests (basic, many, mutex_cond, stress 50 iter).
   - 60 clone events sin PF ni panic.

**Deuda conocida (no bloquea el cierre):**
- [ ] Señales per-thread (blocked_signals compartido entre hilos).
- [ ] kill(tid, sig) / pthread_kill dirigido a un thread.
- [ ] execve desde thread no-líder no mata los demás del team.
- [ ] Futex robusto (FUTEX_LOCK_PI) → -ENOSYS.
- [ ] CLONE_VFORK real.

---

# Estado global

| Bloque | Estado |
|---|---|
| 1 Job control | ✅ |
| 2.1 aurora.conf | ✅ |
| 2.2 pivot_root | ⚠️ infra lista, no en uso |
| 2.3 Layout | ✅ equivalente alcanzado |
| 2.4 aurora-fsck | ✅ |
| 3 VFS/FS completo | ✅ |
| 4 Syscalls relleno | ✅ |
| 5.1 /etc/passwd | ✅ |
| 5.2 init | ✅ |
| 5.3 reaper | ✅ |
| 5.4 getty | ⏳ |
| 6.1 scancodes | ✅ |
| 6.2-6.4 | ⏳ |
| 7 futex+clone | ✅ |

**Lo que queda del TODO**: 5.4 (getty — opcional, cubierto por init),
6.2-6.4 (input/terminal avanzado), y el item opcional de mknod por
major/minor. Todo lo demás está cerrado o equivalente.

**Siguiente bloque sugerido**: Terminal 2D (Fase 4.3 del ROADMAP), que
desbloquea `vi`, `less` y `top` con render completo. Requiere ~8-10 h
de trabajo: matriz de celdas, parser CSI completo, scroll region,
alternate screen, render diferencial y SIGWINCH.