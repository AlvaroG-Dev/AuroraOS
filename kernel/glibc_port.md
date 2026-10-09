# Aurora OS — Estado del port de userspace glibc

Documento de seguimiento. Última actualización tras la sesión que
cerró el bloque interactivo (`nano`/`ed`/`less`), `statx`,
`/proc/<pid>/{cgroup,ctty}` y varios bugs reales de FAT32/VFS
que colgaban el sistema durante los tests de `truncate`.

---

## 1. Estado actual

### Lo que funciona end-to-end

- **bash** interactivo, con prompt, historial y line editing (libtinfo).
- **coreutils completos** (`true echo cat ls cp mv rm mkdir rmdir pwd
  ln readlink realpath touch stat chmod chown wc head tail sort uniq
  cut tr tee kill sleep printf env id whoami uname date dd seq
  basename dirname factor nproc numfmt expand unexpand fold fmt nl tac
  rev split diff cmp comm md5sum sha1sum sha256sum b2sum cksum base32
  base64 dir vdir install mktemp link timeout stdbuf od xxd test expr
  yes shuf`).
- **texto**: `grep egrep fgrep sed awk gawk mawk uniq paste join csplit
  tsort diff3 sdiff`.
- **compresión**: `gzip gunzip zcat bzip2 bunzip2 bzcat xz unxz xzcat
  tar unzip`.
- **filesystem**: `find` con fchdir, `du -sh`, `stat -f`, `df -h`.
- **terminal**: `less`, `nano`, `ed` (end-to-end con Ctrl+O / heredoc).
- **compilador**: `tcc -run` compila y ejecuta.
- **frotz + Zork I** (con `-R`).
- **`vmstat`** con `/proc/vmstat`.
- **`ps aux` / `top`** (con `/proc/<pid>/stat` de 52 campos y
  `/proc/<pid>/status` extendido).
- **`pgrep` / `pkill` / `ps -l`** (con `/proc/<pid>/{cgroup,ctty}`).
- **`lsof -p N`** (con `exe_path` real y `<pid>/exe` stat-eable).
- **`pgrep` / `pkill` / `ps -l`** con wake real, sin caer a polling
  interno.
- **perl 5.38** dinámico (glibc), con `Data::Dumper`, `Encode`,
  `POSIX`, `Unicode::*`, `Time::HiRes` y demás módulos XS cargando
  desde `/usr/lib/x86_64-linux-gnu/perl/5.38.2/`.
- **python3 3.12.3** con imports del stdlib (`sys`, `json`, `re`,
  `hashlib`, `os`, `time`) funcionando end-to-end. Carga de extensiones
  C vía `lib-dynload/*.so` operativa.

### Fixes de kernel acumulados

De la sesión grande original:

- **tarfs**: inode único por nodo (era siempre 0 → glibc confundía
  libc con libtinfo y reportaba "GLIBC_2.x not found" falsos).
- **tty**: `struct termios` al ABI Linux x86-64 (36 bytes, NCCS=19).
- **xattr**: `setxattr`/`getxattr`/`listxattr`/... → `-EOPNOTSUPP`.
- **dup3** (292), **fchdir** (81), **symlinkat** (266),
  **gettimeofday**, **time**, **dup**, **getpgrp**, **tgkill**.
- **procfs**: `mountinfo`, `filesystems`, `/proc/<pid>/fd/N` con
  dirfd real en readdir, `<pid>/{cwd,exe,root}`.
- **stat**: `st_dev` único por FS (`fs_dev_id`).
- **mountinfo**: raíz con id=1, parent_id=1, devices únicos.
- **fat32**: `statfs` real (lee free_clusters del FAT cache).
- **elf.c**: zeroing de la última página de cada PT_LOAD.
- **pf.c**: `MAP_FIXED_NOREPLACE`, `length` desalineado, excepciones
  ring 3 matan al proceso, no al kernel.
- **panic handler**: excepciones de userspace no tiran el kernel.
- **tarfs**: resolver symlinks mid-path cuando el symlink no es el
  último componente (`5.38/Data/Dumper.pm` donde `5.38 -> 5.38.2`).
  El bucle de resolución iterativa estaba implementado pero el
  buffer `work` se corrompía al reescribirlo: el resto del path
  apuntaba dentro del propio buffer y el primer `memcpy` pisaba los
  bytes que el segundo todavía tenía que leer. Fix: copiar el resto
  a un buffer temporal antes de sobrescribir `work`.

De la sesión interactiva / statx:

- **`/proc/vmstat`** con ~100 claves; `vmstat` funciona end-to-end.
- **`/proc/<pid>/status`** extendido: Umask, FDSize, Groups,
  VmPeak/HWM/Size/RSS/Data/Stk/Exe/Lib, RssAnon/File, Threads,
  SigQ/Pnd/Blk/Ign/Cgt, Cpus_allowed(+list), Mems_allowed(+list).
- **`/proc/<pid>/stat`** con 52 campos al layout Linux x86_64
  (tty_nr, tpgid, flags, minflt/cminflt/majflt/cmajflt, utime/stime,
  cutime/cstime, priority/nice, num_threads, itrealvalue, starttime,
  vsize/rss/rsslim, startcode/endcode, startstack, kstkesp/kstkeip,
  signal/blocked/sigignore/sigcatch, wchan, nswap/cnswap,
  exit_signal, processor, rt_priority, policy,
  delayacct_blkio_ticks, guest_time/cguest_time, start_data/end_data,
  start_brk, arg_start/end, env_start/end, exit_code).
- **`/proc/<pid>/{cgroup,ctty}`**: cgroup vacío v2 (`0::/`) y ctty
  symlink al path del terminal de control (`/dev/pts/N` o
  `/dev/console`), fichero vacío si no hay ctty.
- **`statx` (332)** completo (AT_FDCWD, dirfd, AT_SYMLINK_NOFOLLOW,
  AT_EMPTY_PATH). Sin él, coreutils 9.x disparaba un SYSCALL-GAP
  por cada `ls`.
- **`exe_path[VFS_PATH_MAX]` en `process_t`**, rellenado en
  `execve`, usado por `<pid>/exe` y `maps`/`smaps`.
- **`exe_file` en `process_t`**: nodo del binario con ref extra,
  coherente con `mm->exe_file` de Linux. Heredado en fork, liberado
  en `process_exit`.
- **`vma->file_node`** (con ref) paralelo a `file_fd`. Unifica el
  camino de `dev`/`inode` en `maps`/`smaps` y sobrevive a un unlink
  del binario.
- **VMA-per-PT_LOAD**: un `vma_t` por cada `PT_LOAD` con sus
  permisos reales (`r--p`, `r-xp`, `r--p`, `rw-p`). `pmap`, `gdb`,
  `lsof` parsean `maps` como en Linux.
- **`/etc/ld.so.cache`** generado en build time con
  `ldconfig -r sysroot`. Reduce a la mitad los OPEN-FAIL por exec.
- **Bind mount `/tmp` → `/data/tmp`** en `kmain_task` (después del
  mount de `/data`). `/tmp` es RW y stat-able desde todos los
  procesos.
- **`TMPDIR=/data/tmp`** en el env de `init`. `ed` funciona con
  heredoc/pipe; antes fallaba con `?` porque glibc no encontraba
  dónde crear su scratch.
- **`O_TMPFILE` → `-ENOENT`** (no `-EOPNOTSUPP` ni `-EISDIR`).
  glibc en `sysdeps/posix/tempname.c` solo cae a su fallback de
  `mkstemp` con EISDIR o ENOENT; ENOENT es la constante universal.
  El check es de conjunción (`== 0x410000`), no disyunción, para
  no rechazar `O_DIRECTORY` solo (que pasa `ls .`/`ls /`).
- **`terminal.c`**: `case 'c'` (DA1/DA2) solo responde si no hay
  params. Las responses del propio terminal llegando por el eco del
  slave se parseaban como nueva query y producían `6c6c6c...`
  infinitos. `nano` inicializando ncurses lo disparaba.
- **`terminal.c`**: `case 'q'` (DECSCUSR) comprueba `inter[0]==' '`,
  no `private_marker==' '`. El SP es intermediate byte.
- **`vfs_node_free`**: el early-return `if (new_rc >= 0) return;`
  impedía liberar ningún nodo. Con `ref_count` empezando en 1,
  todos los nodos se filtraban y el SLAB reciclaba direcciones de
  `fat32_fs_t` ya liberados por `fat32_umount`, dejando la mount
  table apuntando a memoria ajena. Fix: `if (new_rc > 0) return;`.
- **`fat32_mutex_lock/unlock`**: `unlock` cogía `m->waiters.lock`
  a mano (violación de encapsulación de `wait_queue_t`). Ahora
  delega en `wake_up_all`. Lock usa `exchange` en vez de CAS.
- **Tres sitios creaban `vfs_node_t` sin `ref_count=1`**:
  `pty.c:pty_install_fd`, `pipe.c:nodos read/write end`,
  `vfs.c:fallback root`. El underflow exponía el bug de
  `vfs_node_free`.
- **`k_sendfile`**: buffer de 4 KB movido a `kmalloc` (frame de
  4208 B disparaba `-Wframe-larger-than=2048`).
- **`pty_m_has_space`** eliminado (dead code desde que el
  slave→master pasó de back-pressure a drop silencioso).

---

## 2. Pendientes inmediatos

Los items originales de esta sección quedaron cerrados:

- 2.1 `/proc/vmstat` ✅
- 2.2 `pgrep` ✅ (era falso positivo; `pgrep sh` devuelve 3, rc=0)
- 2.3 `/proc/<pid>/status` ✅
- 2.4 `exe_path` + `exe_file` + `vma->file_node` ✅
- 2.5 `less`/`nano`/`ed` interactivos ✅
- 2.6 `tmux`/`screen` → bloqueado por `epoll`, ver §3.9
- 2.7 `awk`/`gawk`/`mawk` ✅
- 2.8 `perl` ✅ / `python3` → pendiente, ver §5.1

---

## 3. Pendientes medianos (1-2 tardes cada uno)

### 3.1 `/proc/self/mounts` coherente con `/proc/self/mountinfo`

Pendiente. La mountinfo ya funciona; mounts está aceptable pero
podría normalizarse.

### 3.2 `/proc/<pid>/maps` con `exe_path` real

✅ Cerrado con `exe_path` + `exe_file` + `vma->file_node` +
VMA-per-PT_LOAD.

### 3.3 `/proc/sys/` más completo

Pendiente: `pid_max`, `threads-max`, `random/{uuid,boot_id}`,
`vm/overcommit_memory`, `vm/max_map_count`.

### 3.4 `/proc/meminfo` con más campos

Pendiente: `Active`, `Inactive`, `SwapCached`, `Mapped`,
`Committed_AS`. Los usa `free` de procps.

### 3.5 `statfs` para tarfs con bloques reales

Opcional.

### 3.6 `sysinfo(2)` más completo

Pendiente: `loads[]` y `sharedram`.

### 3.7 `uname -a` — `processor`/`hardware-platform`

Cosmético. Dejar como está.

### 3.8 Señales reales

Pendiente: `signalfd`, `sigaltstack` con `SA_ONSTACK`,
`pidfd_send_signal`. Además, como deuda latente identificada
durante el fix de FAT32: **filtrar señales ignoradas en
`signal_check_pending` y en `wait_common`** (Linux lo hace; no
manifestado en la práctica, pero correcto).

### 3.9 `epoll` — ✅ CERRADO

`epoll_create1`/`epoll_ctl`/`epoll_wait` + `epoll_pwait`/`epoll_pwait2`
implementados en `kernel/epoll.c`. Wake real vía cascada de wait
queues (`wait_queue_t::subs`): epoll subscribe su wq a la `poll_wq`
natural de cada fd observado. Latencia de wakeup medida en 230 ms
con `usleep(200)` en el writer → wake inmediato.

Sin soporte de `EPOLLET`, `EPOLLONESHOT`, `EPOLLEXCLUSIVE` (flags
aceptados, ignorados).

### 3.10 `eventfd` — ✅ CERRADO

`eventfd` / `eventfd2` con `EFD_SEMAPHORE` / `EFD_NONBLOCK` /
`EFD_CLOEXEC`. Expone `poll_wq` para epoll.

### 3.11 `signalfd` — ✅ CERRADO

`signalfd` / `signalfd4`. Mask bloqueado en el proceso al crear.
`signalfd_notify()` se llama desde `process_signal_pid_ex`, `k_kill`
y `process_signal_pgrp` cuando se encola una señal.
`signalfd_cleanup()` desbloquea el mask al salir el proceso.

---

## 4. Objetivos medios

### 4.1 Red: loopback + stack IP mínimo

Sin cambios.

### 4.2 `vDSO`

Sin cambios.

### 4.3 `clone3` y `rseq` de verdad

Sin cambios. Ahora `rseq` sigue devolviendo `-ENOSYS` y `clone3`
cae a `clone` (ver SYSCALL-ERR num=334 en todos los execs).

### 4.4 Usuarios reales

`/etc/passwd`, `/etc/group`, `/etc/shadow` ya funcionan.
Pendiente: `setuid` real irreversible, `getpwnam` con NSS.

### 4.5 `ptrace` mínimo

Sin cambios.

---

## 5. Objetivos grandes (un mes+)

### 5.1 Portar `python3` ✅ (intérprete básico operativo)

**3.12.3 funcional.** Ejecuta scripts, importa stdlib, compila bytecode
al vuelo, carga extensiones C. Pendiente afinar:

- `sigaltstack` (131) — usado por `faulthandler`.
- `rt_sigaction` devuelve `-EINVAL` en 3 llamadas al arrancar (probable
  bug en la comprobación de `sigsetsize` o en la entrega a userland).
  No bloquea pero ensucia el log.
- `ioctl(TCGETS)` sobre PTY devuelve `-ENOTTY` al cargar `encodings/utf_8`.
  Tampoco bloquea pero convendría revisar `tty_ioctl`.
- Compilar `.pyc` con `compileall` en build time para acelerar arranque.

### 5.2 Portar `git`

Sin cambios.

### 5.3 Portar `gcc` nativo

Sin cambios.

### 5.4 Soporte de editores gráficos

Sin cambios.

### 5.5 Servidor gráfico tipo Wayland o X11

Sin cambios.

### 5.6 Networking avanzado

Sin cambios.

---

## 6. Deuda técnica y refactor pendiente

### 6.1 Logs de diagnóstico

La mayoría de los logs ruidosos de `syscall.c` se eliminaron en
sesiones anteriores. Quedan activos:
- `[SYSCALL-ERR]` — útil, se mantiene.
- `[SYSCALL-GAP]` — útil, se mantiene.
- `[OPEN-FAIL]` — útil, se mantiene.
- `[uaccess fixup]` — útil, se mantiene.
- `[PF] Matando` — útil, se mantiene.
- `[SIG-DBG]` en `signal_check_pending` — **temporal**, quitar
  cuando se cierre la deuda de §3.8.

### 6.2 `fs_dev_id` es de 16 slots

Sin cambios. Aceptable.

### 6.3 `p->name` vs `exe_path`

✅ Cerrado con `exe_path`.

### 6.4 Syscall `statx`

✅ Cerrado.

### 6.5 `/etc/ld.so.cache`

✅ Cerrado. Se genera con `ldconfig -r sysroot` en build time.

---

## 7. Backlog ordenado por prioridad

| # | Item | Esfuerzo | Prioridad |
|---|---|---|---|
| 1 | ~~Probar `perl`~~ ✅ | — | **Hecho** |
| 2 | Probar `python3` (falta stdlib) | 2-4 h | Alta |
| 3 | `/proc/sys/` más completo | 2 h | Media |
| 4 | `/proc/meminfo` completo | 1 h | Media |
| 5 | Filtrar señales ignoradas en wait/signal_check (deuda latente) | 1 h | Media |
| 6 | `clone3` + `rseq` | 1 día | Media |
| 7 | Red loopback | 2 días | Alta |
| 8 | Driver de red (`e1000`) | 3 días | Alta |
| 9 | TCP funcional | 1 semana | Alta |
| 10 | Usuarios reales (`setuid` irreversible) | 1 día | Media |
| 11 | Portar `git` | 1 semana | Media |
| 12 | vDSO | 2 días | Baja |
| 13 | `ptrace` (subset) | 3 días | Baja |
| 14 | Portar `gcc` | 1 mes+ | Baja |

---

## 8. Cómo probar tras cada cambio

Cada vez que toques `syscall.c`, `procfs.c`, `pf.c` o `vfs.c`:

```bash
make -C kernel 2>&1 | grep -iE "error|warning: implicit" | head
make image
make run-smp-kvm-ahci
En el guest, mini-smoke:

sh
/usr/bin/df -h
/usr/bin/df -h -a
/usr/bin/stat -f /data
/usr/bin/ls -la /
/usr/bin/cat /proc/self/mountinfo
/usr/bin/free
/usr/bin/uptime
/usr/bin/du -sh /etc
Y para regresiones específicas:

sh
/usr/bin/bash /smoke.sh
(ver §9.1 si lo integras en el initrd).

9. Tooling pendiente
9.1 Integrar scripts/smoke.sh en el Makefile
Pendiente.

9.2 Tabla de símbolos del kernel
Pendiente. Ahorra horas de objdump/nm en cada panic.

9.3 dmesg en el guest ya funciona
dmesg | tail -30 lee el ring de klog.

10. Referencias útiles
Linux ABI x86_64: arch/x86/entry/syscalls/syscall_64.tbl

glibc sysdeps: sourceware.org/git/?p=glibc.git

man pages: man 2 <syscall>

Testing: tests/kernel_tests.c como patrón

Notas
Este documento cubre solo el port de userspace glibc. Otros
subsistemas de Aurora tienen su propio backlog.

El bloque interactivo + statx + cgroup/ctty + fixes de FAT32 se
cerró en la sesión de 2024-XX-XX (ver TODO.md Bloque 9).

El siguiente bloque es epoll + eventfd, que desbloquea
tmux/screen/python3/perl/libuv.