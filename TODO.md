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

Bloque 8 — Cierre de SMP estable + syscalls Linux ✅ CERRADO

8.1 Syscalls Linux adicionales ✅
   - klogctl(103): dmesg funciona (`dmesg`, `dmesg -c`, `dmesg | head`).
   - flock(73): advisory lock por fd con scan de todos los procesos.
   - waitid(247): wrapper de process_waitpid + relleno de siginfo_t.
   - setdomainname(171): cosmético, lo devuelve uname().
   Cierra el ⚠️ de 3.5 del ROADMAP (superficie syscall para BusyBox).

8.2 Bugs SMP reales cazados con `dmesg | wc -c` ✅
   - **Deadlock de `ipi_tlb_shootdown`**: dos CPUs concurrentes en
     ld.so (mprotect RELRO). CPU A tenía `tlb_shootdown_lock` esperando
     acks; CPU B giraba en el mismo lock sin poder atender la IPI que
     A le mandaba. Deadlock mutuo. Fix: `tlb_pending` bitmask + 
     `tlb_service` por polling + `preempt_disable` durante el protocolo.
   - **`isr_handler` con `saved_regs` static**: dos CPUs en #PF
     concurrente corrompían el frame la una de la otra. Fix: usar el
     frame propio (parámetro `regs`).
   - **Ventana física en UC vs WB**: la ventana directa mapeaba RAM
     como NOCACHE mientras las páginas de usuario mapeaban los mismos
     frames como WB. En x86 esto es aliasing indefinido. Fix: ventana
     como Write-Back.
   - **`ref_count` de file_descriptor_t no atómico**: `dup2`, `fork`,
     clonado de VMAs y `vfs_close_for_proc` usaban `++`/`--` sin
     `__atomic_*`. Cuando dmesg/wc/shell cierran el mismo fd a la vez,
     se perdían decrementos y el write end del pipe nunca llegaba a 0.
   - **Use-after-free en `pipe_close`**: dos extremos cerrando a la vez
     en CPUs distintas hacían `wake_up_all(&p->...)` sobre `pipe_t`
     ya liberado. Fix: contador `refs` (2 al crear) + `kfree` solo
     cuando llega a 0.
   - **`FD_CLOEXEC` no implementado**: `k_fcntl(F_SETFD)`,
     `k_dup2` y `execve` ignoraban el bit. Busybox ash filtraba
     extremos de pipe sobrantes. Fix: `fd_cloexec_mask` por proceso,
     `execve` cierra los marcados.

8.3 Regresión ✅
   - `dmesg | wc -c` × 15 iteraciones × 3 paralelos con `-smp 2` y
     `-smp 4` sin cuelgues.
   - `test_thread` sigue OK después de todos los cambios.

**Deuda conocida (no bloquea el cierre):**
- [ ] Señales per-thread (blocked_signals compartido entre hilos).
- [ ] `kill(tid, sig)` / `pthread_kill` dirigido a un thread.
- [ ] `execve` desde thread no-líder no mata los demás del team.
- [ ] Futex robusto (`FUTEX_LOCK_PI`, `FUTEX_REQUEUE`, `FUTEX_WAKE_OP`).
- [ ] `CLONE_VFORK` real.
- [ ] `mknod` en devfs por major/minor (hoy solo guarda rdev).

### Bloque 9 — Cierre del bloque interactivo + fixes estructurales ✅ CERRADO

9.1 `/proc/vmstat` ✅
   `gen_vmstat()` con ~100 claves a 0. `vmstat` funciona end-to-end.

9.2 `/proc/<pid>/status` extendido ✅
   Añadidos: Umask, FDSize, Groups, VmPeak/HWM/Size/RSS/Data/Stk/
   Exe/Lib, RssAnon/File, Threads, SigQ/Pnd/Blk/Ign/Cgt,
   Cpus_allowed(+list), Mems_allowed(+list).

9.3 `/proc/<pid>/stat` con 52 campos ✅
   Layout Linux x86_64 completo. Necesario para libproc2 (`ps aux`,
   `top`). Requirió añadir 9 campos a `process_t`: start_tick,
   start_code, end_code, start_data, end_data, arg_start, arg_end,
   env_start, env_end. `calc_elf_code_data_ranges()` en process.c
   los rellena a partir de los `elf_segment_t`.

9.4 `exe_path` + `exe_file` + `vma->file_node` ✅
   - `vfs_node_t` gana refcount real (`vfs_node_ref` + liberación
     efectiva en `vfs_node_free`).
   - `process_t.exe_path[VFS_PATH_MAX]` y `process_t.exe_file`
     (ref extra, coherente con `mm->exe_file`). Heredado en fork,
     liberado en `process_exit`.
   - `vma_t.file_node` paralelo a `file_fd`. `vma_inherit_backing()`
     centraliza la copia con refs correctos.
   - VMA-per-PT_LOAD: un `vma_t` por segmento con sus permisos
     reales. `maps`/`smaps` parseables por `pmap`, `gdb`, `lsof`.

9.5 `/etc/ld.so.cache` ✅
   Generado con `ldconfig -r sysroot` en build time. Corta a la
   mitad los OPEN-FAIL por exec.

9.6 `statx` (332) ✅
   Estructura de 256 bytes, `vfs_to_statx()`, soporte de AT_FDCWD,
   dirfd, AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH. Elimina el
   `[SYSCALL-GAP] num=332` que coreutils 9.x dispara en cada `ls`.

9.7 `/proc/<pid>/{cgroup,ctty}` ✅
   - `cgroup`: `0::/\n`.
   - `ctty`: symlink a `/dev/pts/N` o `/dev/console`. Fichero
     vacío si no hay ctty. Implementado con `tty_get_path()` en
     pty.c, declarado en tty.h.
   - Añadidos a `procfs_pid_entries[]` para readdir.

9.8 Fixes interactivos ✅
   - `terminal.c` `case 'c'` (DA1): solo responde si no hay
     params. Evita el bucle `6c6c6c...` con `nano`.
   - `terminal.c` `case 'q'` (DECSCUSR): comprueba `inter[0]==' '`,
     no `private_marker`. El SP es intermediate byte.
   - `O_TMPFILE`: `-ENOENT` (no `-EISDIR` ni `-EOPNOTSUPP`).
     glibc cae a `mkstemp` con ENOENT o EISDIR; ENOENT es la
     constante universal.
   - `TMPDIR=/data/tmp` en `default_envp[]` de init.
   - Bind mount `/tmp` → `/data/tmp` en `kmain_task` (después
     del mount de `/data`, no en `vfs_init`).

9.9 Fixes estructurales descubiertos en el camino ✅
   - **`vfs_node_free` nunca liberaba**: `if (new_rc >= 0) return;`
     con `ref_count` inicial 1 → siempre salía por early-return.
     Todos los nodos se filtraban. `fat32_test_cleanup()` liberaba
     el `fat32_fs_t` y el SLAB reciclaba su dirección para otro
     objeto, dejando la mount table con `fs_priv` apuntando a
     memoria ajena. Hang silencioso en `fat32_mutex_unlock`.
     Fix: `if (new_rc > 0) return;`.
   - **`fat32_mutex_unlock` cogía `m->waiters.lock` a mano**:
     violación de encapsulación de `wait_queue_t`. Delegado en
     `wake_up_all`. Lock simplificado a `exchange`.
   - **Tres sitios creaban `vfs_node_t` sin `ref_count=1`**:
     `pty.c:pty_install_fd`, `pipe.c` (read/write end),
     `vfs.c` (fallback root). Underflow exponía el bug anterior.
   - **`k_sendfile`** con `uint8_t buf[4096]` en pila: frame de
     4208 B, `-Wframe-larger-than=2048`. Movido a `kmalloc`.
   - **`pty_m_has_space`** eliminado (dead code).

**Deuda latente identificada pero no manifestada:**
- [ ] Filtrar señales ignoradas en `signal_check_pending` y en
      `wait_common` (Linux lo hace; evita bucles `read → EINTR` si
      una señal con SIG_IGN queda pendiente).
- [ ] Quitar el `[SIG-DBG]` temporal de `signal_check_pending`.
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
| 5.4 getty | ⏳ (opcional, cubierto por init) |
| 6.1 scancodes | ✅ |
| 6.2-6.4 | ⏳ |
| 7 futex+clone | ✅ |
| 8 SMP + syscalls Linux | ✅ |
| 9 Interactivas + statx + /proc extendido + fixes FAT32 | ✅ |

**Lo que queda del TODO**:
- 5.4 (getty) — opcional, ya cubierto por init.
- 6.2-6.4 (copy/paste, cursor parpadeante, serial como consola).
- Deuda latente de señales ignoradas en wait/signal_check.

**Siguiente bloque sugerido**: `epoll` + `eventfd` (Fase 3.9 del
glibc_port, ~4 h). Desbloquea `tmux`, `screen`, `python3` asyncio,
`libuv`, y hace que `pgrep` deje de emitir SYSCALL-GAP 213.