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

### Bloque 10 — epoll + eventfd + signalfd ✅ CERRADO

10.1 `epoll_create` (213) / `epoll_create1` (291) ✅
    `kernel/epoll.c` nuevo. Instance con `wait_queue_t` interna y
    lista de entries.

10.2 `epoll_ctl` (233) ✅
    ADD/MOD/DEL. En ADD suscribe `ep->wq` a la `poll_wq` natural del
    fd observado (o `fd->read_wq` como fallback). En DEL/close
    desuscribe.

10.3 `epoll_wait` (232) / `epoll_pwait` (281) / `epoll_pwait2` (441) ✅
    Wake real vía cascada de `wait_queue_t::subs`: cuando el fd
    observado se vuelve listo, su wq cascada a `ep->wq`, que
    despierta al waiter. Sin polling. Latencia del test: 230 ms
    con `usleep(200)` en el writer → wake inmediato.

10.4 `eventfd` (284) / `eventfd2` (290) ✅
    Contador 64-bit, EFD_SEMAPHORE, EFD_NONBLOCK, EFD_CLOEXEC.
    Expone `poll_wq` para que epoll se suscriba.

10.5 `signalfd` (282) / `signalfd4` (289) ✅
    Mask bloqueado en el proceso al crear. read() consume
    pending_signals y devuelve `struct signalfd_siginfo` (128 B).
    `signalfd_notify()` despierta los signalfds del proceso cuando
    se encola una señal.

10.6 Infraestructura: `wait_queue_t::subs` ✅
    Nuevo campo (lista de suscriptores). `wake_up_all_locked` y
    `wake_up_one_locked` cascadan a los suscriptores. Un solo nivel
    (no recursivo), suficiente para epoll.

10.7 Fix SMAP en ops de nodo ✅
    `eventfd_read_op` / `eventfd_write_op` / `signalfd_read_op`
    hacían `copy_to_user`/`copy_from_user` sobre el buffer que el
    VFS ya había copiado a kernel. Con SMAP activo, `access_ok`
    falla sobre direcciones del kernel y devuelve `-EFAULT`.
    El log lo cazó como `SYSCALL-ERR num=1 ret=-14`. Fix: `memcpy`
    puro, que es lo que hacen el resto de los nodos
    (`pty_master_write_op`, `tty_*`).

**Deuda conocida (no bloquea el cierre):**
- [ ] `EPOLLET` (edge-triggered) — se acepta el flag, se ignora.
- [ ] `EPOLLONESHOT` / `EPOLLEXCLUSIVE` — ignorados.
- [ ] `EPOLLWAKEUP` — ignorado.
- [ ] `epoll_pwait` no es atómico (Linux lo es). Ventana de race
      microscópica entre cambio de máscara y entrada en wait.

### Bloque 11 — Intérpretes dinámicos (perl) ✅ CERRADO

11.1 Fix tarfs symlink mid-path ✅
    `tarfs_fs_lookup` corrompía el buffer `work` al reconstruir el
    path tras resolver un symlink intermedio: el resto del path
    apuntaba dentro de `work` y el primer `memcpy` pisaba bytes que
    el segundo aún no había leído. Fix: buffer temporal `rest_buf`.
    Cierra `perl -MData::Dumper`.

11.2 SIG_MAX = 64 confirmado ✅
    Sin `SYSCALL-ERR num=13` en el arranque tras rebuild completo.
    (El header no se recompilaba por falta de dependency tracking;
    ver Bloque 8.)

### Bloque 12 — Python 3.12 + memfd + fixes de kernel ✅ CERRADO

12.1 memfd_create (319) real ✅
    Backed por kmalloc. Read/write posicionales, truncate, mmap
    file-backed, F_ADD_SEALS/F_GET_SEALS. Necesario para bytecode
    caches de Python.

12.2 /proc/meminfo completo ✅
    ~30 claves adicionales que glibc/procps/Python esperan.

12.3 Fix sys_mmap (bug de debug) ✅
    Sanity check mal ubicado leía `base` sin inicializar y disparaba
    #PF en modo kernel con CR2 fuera de la ventana física. Bug
    latente desde que se añadió; Python fue el primer programa en
    ejercerlo por hacer muchos mmap anónimos sucesivos.

12.4 Quitar [SIG-DBG] ✅

12.5 scripts/fetch-python-stdlib.sh ✅

12.6 Test test_memfd ✅

**Deuda latente identificada (no bloquea):**
- [ ] `sigaltstack` (131) → -ENOSYS. Python lo usa vía faulthandler.
- [ ] `rt_sigaction` → -EINVAL en 3 llamadas consecutivas al arrancar
      Python. Loguear `sig` y `sigsetsize` cuando devuelve EINVAL para
      diagnosticar.
- [ ] `ioctl(TCGETS)` sobre PTY → -ENOTTY. Revisar tty_ioctl.
- [ ] Compilar stdlib a .pyc en build time.
- [ ] Limpiar kernel_pml4[0] heredado del bootloader UEFI (excepto
      trampoline SMP en 0x7000-0x9000). Fue un cómplice del bug 12.3.
      
### Bloque 13 — Portar git + fixes AHCI/completion/TLS/FAT32 ✅ CERRADO

13.1 clone3 (435) ✅
    Cae a clone(2) clásico. CLONE_PIDFD/CLONE_INTO_CGROUP
    rechazados con EINVAL (glibc cae a clone sin más).
    set_tid/set_tid_size ignorados.

13.2 futex WAIT_BITSET/WAKE_BITSET ✅
    Implementados sobre el mismo hash que WAIT/WAKE.
    Ignoran el bitset de match (glibc usa MATCH_ANY). No
    rechazan FUTEX_CLOCK_REALTIME (glibc lo pasa
    incondicionalmente; sin él hacía abort() con -ENOSYS).

13.3 /proc/sys completo ✅
    kernel/{pid_max, threads-max, osrelease, random/{uuid,boot_id}}
    vm/{overcommit_memory, max_map_count}. + los existentes.

13.4 madvise(MADV_DONTNEED/MADV_FREE) real ✅
    Libera frames de páginas dentro de VMA. Las páginas fuera
    de VMA (brk) no se tocan para no matar al proceso.

13.5 /dev/urandom, /dev/random ✅
    xorshift64 sembrado con RDTSC. Git los usa para nombres de
    tempfiles; sin ellos `git add` abortaba con "unable to get
    random bytes".

13.6 unlinkat/mkdirat/renameat con dirfd real ✅
    Resuelven path relativo al cwd del fd. Sin esto `rm -rf`
    fallaba con EINVAL en cada hijo y git no podía limpiar su
    índice.

13.7 FAT32 dotfiles ✅
    `.git`, `.bashrc`, etc. van por LFN con alias ~N. Antes
    fat32_is_pure_83 rechazaba cualquier nombre con punto
    inicial → git init no podía crear .git/config.

13.8 FAT32 chmod/chown no-op ✅
    Devuelven 0 (como Linux vfat). Devolver -EPERM rompía git
    (chmod sobre .git/config.lock) y rsync.

13.9 FAT32 rename sobrescribe destino ✅
    POSIX semantics. Sin esto git config fallaba con EEXIST al
    renombrar config.lock → config.

13.10 FAT32 find_free_dirents 0xE5 tail ✅
    Al no caber la racha de slots en el cluster actual, marca
    el tail como 0xE5 antes de saltar al siguiente. Sin esto
    la cola a 0x00 hacía que iter_dir dejara de leer y las
    entradas del siguiente cluster quedaran invisibles.

13.11 FAT32 iter_dir tolerante a 0x00 ✅
    Recupera dirs dañados por versiones anteriores.

13.12 AHCI completion: separar wake directo vs epoll subs ✅
    La wait_queue privada de una completion no debe propagar
    a subs. Hacerlo disparaba #GP cuando el subs era basura de
    memoria reciclada. complete() consume la completion al
    despertar el waiter.

13.13 wait_for_completion_uninterruptible() ✅
    Usado por bdev_read/write/flush. Sin esto, una señal
    (SIGCHLD) hacía que bdev_submit_sync retornara con el bio
    aún en vuelo → #GP con RDX = "/dev/ura" en el panic.

13.14 TLS canonicity en switch.asm + arch_prctl ✅
    switch.asm valida fs_base antes de wrmsr(MSR_FS_BASE) en
    task_switch y task_jump_to. arch_prctl(ARCH_SET_FS)
    rechaza valores no canónicos con -EPERM.

13.15 Terminal: setenv siempre ✅
    Los defaults se aplican sin el `if (!environ)`. Añadidos
    GIT_PAGER=cat, GIT_CONFIG_GLOBAL=/dev/null,
    GIT_CONFIG_SYSTEM=/dev/null, TMPDIR.

13.16 Build: aurora.img persistente ✅
    `make image` ya no reformatea si aurora.img existe. Solo
    actualiza ESP/EFI, kernel.elf, etc/. `/data` sobrevive a
    los rebuilds. `make clean-data` para formatear.

**Verificación:** git 2.43.0 `init` + `add .` (103 ficheros
con LFN, subdirs anidados, binario 256 KB) + `commit` + `log`
+ `status` + `diff` funcionando end-to-end sin panic.

**Deuda conocida (no bloquea el cierre):**
- [ ] `libffi.so.8` falta → ctypes/cffi de Python.
- [ ] `setitimer` (38) → -ENOSYS. Cosmético, ruido en log.
- [ ] `[FAT32-UTIMES]` en LOG_INFO (debería ser TRACE).
- [ ] `sigaltstack` (131) → -ENOSYS.
- [ ] `rt_sigaction` (13) → -EINVAL en 3 llamadas al arrancar Python.
- [ ] `ioctl(TCGETS)` sobre PTY → -ENOTTY.
- [ ] Señales per-thread (blocked_signals compartido entre hilos).

### Bloque 14 — vDSO ✅ CERRADO

14.1 blob ELF (vdso.S + vdso.ld + vdso.map) construido con toolchain del
     host y embebido como vdso_blob.o.
14.2 kernel/vdso.c: pagina vvar (seqlock) + mapeo del blob en el pml4.
     PTE_SPECIAL para que paging_free_user_space no libere las paginas
     compartidas al terminar un proceso.
14.3 time.c: vdso_update_clock() en cada tick desde CPU0.
14.4 process.c: mapea vdso en spawn y execve, AT_SYSINFO_EHDR (33) en
     auxv. Reserva 19 entradas en lugar de 18.
14.5 vdso.S: __vdso_gettimeofday dividia en vez de multiplicar para
     tv_usec.

**Verificado:** 100k time.monotonic() en 4.0 ms (~40 ns/llamada).
**Efecto colateral positivo:** el doble-free de PMM al morir un proceso
     era este bug (vvar/vdso liberados como privados).

### Bloque 15 — tmpfs ✅ CERRADO

15.1 kernel/tmpfs.{h,c}: nuevo FS en RAM. Nodos con buffer kmalloc'd,
     directorio con lista enlazada. Cota global por instancia (ENOSPC).
     Un mutex por FS. Refcounts nlink/vfs_refs (POSIX unlink semantics).
     Symlinks.
15.2 tmpfs_init(): monta /etc (4 MB), /tmp (64 MB), /var (16 MB),
     /run (4 MB). Antes de shadow-ear /etc, lee /etc/ld.so.cache de
     tarfs y lo reescribe dentro del tmpfs.
15.3 vfs_init() llama a tmpfs_init() al final.
15.4 main.c: elimina el bind mount /tmp -> /data/tmp (obsoleto).
15.5 Makefile raiz: /tmp, /var, /run ya no se crean en tarfs. /etc
     solo lleva ld.so.cache. passwd/group/profile en runtime.

**Verificado:** /tmp, /var, /etc, /run escribibles y persistentes
mientras el sistema esta arriba. /etc/ld.so.cache sobrevive al mount
tmpfs (leido de tarfs en boot).
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
| 10 epoll + eventfd + signalfd | ✅ |
| 11 Intérpretes dinámicos (perl) | ✅ |
| 12 Python | ✅ |
| 13 Portar git + AHCI/completion/TLS/FAT32 | ✅ |
| 14 vDSO (clock_gettime/gettimeofday/time) | ✅ |
| 15 tmpfs (/etc, /tmp, /var, /run) | ✅ |

**Lo que queda del TODO**:
- 5.4 (getty) — opcional, ya cubierto por init.
- 6.2-6.4 (copy/paste, cursor parpadeante, serial como consola).
- Deuda latente de señales ignoradas en wait/signal_check.

**Siguiente bloque sugerido**: `epoll` + `eventfd` (Fase 3.9 del
glibc_port, ~4 h). Desbloquea `tmux`, `screen`, `python3` asyncio,
`libuv`, y hace que `pgrep` deje de emitir SYSCALL-GAP 213.