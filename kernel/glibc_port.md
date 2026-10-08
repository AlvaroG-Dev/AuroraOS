# Aurora OS — Estado del port de userspace glibc

Documento de seguimiento. Última actualización tras la sesión que
cerró `df`, `du`, `find` con dirfd, `vma_isolation` y varios fixes de
procfs/tarfs.

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
- **filesystem**: `find` con fchdir, `du -sh`, `stat -f`, `df -h`
  (tras fix de st_dev + mountinfo).
- **terminal**: `less`, `nano`, `ed` (parcialmente, ver pendientes).
- **compilador**: `tcc -run` compila y ejecuta.
- **frotz + Zork I** (con `-R`).

### Fixes de kernel acumulados en la sesión grande

- **tarfs**: inode único por nodo (era siempre 0 → glibc confundía
  libc con libtinfo y reportaba "GLIBC_2.x not found" falsos).
- **tty**: `struct termios` al ABI Linux x86-64 (36 bytes, NCCS=19).
- **xattr**: `setxattr`/`getxattr`/`listxattr`/... → `-EOPNOTSUPP` para
  que `ls -l` no muestre "Function not implemented".
- **dup3** (292), **fchdir** (81), **symlinkat** (266),
  **gettimeofday**, **time**, **dup**, **getpgrp**, **tgkill**.
- **procfs**: `mountinfo`, `filesystems`, `/proc/<pid>/fd/N` con
  dirfd real en readdir, `<pid>/{cwd,exe,root}` correctos.
- **stat**: `st_dev` único por FS (`fs_dev_id`) para que `df` no
  deduplique todos los mounts.
- **mountinfo**: raíz con id=1, parent_id=1, device únicos por FS.
- **fat32**: `statfs` real (lee free_clusters del FAT cache).
- **elf.c**: zeroing de la última página de cada PT_LOAD (fix del
  stack smash con `cat-glibc` en runs 2+).
- **pf.c**: `MAP_FIXED_NOREPLACE`, `length` desalineado, excepciones
  ring 3 que matan al proceso en lugar del kernel.
- **panic handler**: ahora excepciones de userspace no tiran el kernel.

---

## 2. Pendientes inmediatos (bloque actual)

Estos son los "siguientes 5 minutos". Cada uno es pequeño y localizado.

### 2.1 `vmstat` — falta `/proc/vmstat`
vmstat: Unable to create vmstat structure

text

`vmstat` lee `/proc/vmstat` (estadísticas VM: `pgfault`, `pgmajfault`,
`nr_free_pages`, ...) además de `/proc/stat`. Fichero que no existe.

**Acción**: añadir `gen_vmstat()` a `procfs.c` con ~10-20 claves a 0
(las mínimas que `vmstat` espera). Mapearlo en `procfs_lookup`.

### 2.2 `pgrep bash` — imprime la ayuda sin razón

Probablemente `pgrep` no puede leer `/proc/<pid>/stat` o encuentra
basura en algún path. Necesita reproducción en el guest con:
pgrep --version
pgrep -l sh

text

Y serial filtrado con `SYSCALL-ERR|SYSCALL-GAP|OPEN-FAIL`.

### 2.3 `/proc/<pid>/status` incompleto

Falta `VmPeak`, `VmHWM`, `SigQ`, `SigPnd`, `ShdPnd`, `Cpus_allowed`,
etc. `ps aux` y `top` los leen. Añadir los que usan realmente
busybox/procps.

### 2.4 `lsof` a medias

`lsof -p 1` imprime cabecera y fds pero no puede `stat` los `txt` y
`/dev/console`. La causa:
- `p->name` en lugar del path real del ejecutable (`exe_path`).
- El nodo de `/dev/console` en `<pid>/fd/0` no es stat-eable desde
  procfs.

**Acción**:
- Añadir `char exe_path[VFS_PATH_MAX]` a `process_t`, rellenarlo en
  `execve`, y usarlo en `gen_pid_maps_cb` / `gen_pid_smaps_cb` y en
  `<pid>/exe`.
- En `<pid>/fd/N`, devolver el nodo real del fd en lugar de un symlink
  al nombre — o arreglar el readlink para que el symlink sea stat-eable.

### 2.5 `less`/`nano`/`more` — verificar interactivamente

Básicos funcionan (`--version`), pero el path interactivo no se ha
probado. `less /etc/passwd` y `q` para salir; `nano /data/t.txt`,
Ctrl+X para guardar. Pasar por el PTY.

### 2.6 `tmux` / `screen` — casi seguro fallan

Necesitan `eventfd`, `epoll`, sockets Unix, `poll` con muchos fds.
Son un objetivo a medio plazo, no inmediato.

### 2.7 `awk`/`gawk`/`mawk` — verificación de contenido

Solo hemos probado `awk '{print $2}'`. `gawk` con arrays, `split`,
`match`, `gsub` es probable que funcione pero no se ha verificado.
`gawk` pide `libm` (ya está en `libs/glibc/`).

### 2.8 `python3` / `perl` — sin probar

Los binarios están copiados pero sin arrancar. Es el salto grande:
`python3` enlaza con ~40 libs y usa `epoll`, `eventfd`, `mmap` con
flags raros, `clone3`, `rseq`. **Probar primero `perl`** (más
pequeño).

---

## 3. Pendientes medianos (1-2 tardes cada uno)

### 3.1 `/proc/self/mounts` coherente con `/proc/self/mountinfo`

Ahora mismo `gen_mounts` emite `super_opts` = `rw` y `0 0` extra
(compatible con Linux). Pero el parser de `mount -l` y algunos
programas viejos esperan formato `fstype mountpoint` sin más. Revisar
que ambos formatos son válidos.

### 3.2 `/proc/<pid>/maps` con `exe_path` real

Hoy ponemos `p->name` como path del VMA ELF. Con un `exe_path[VFS_PATH_MAX]`
en `process_t`, `maps` y `smaps` muestran la ruta real, y los
debuggers (`gdb` en un futuro) pueden resolver mejor.

### 3.3 `/proc/sys/` más completo

Añadir:
- `/proc/sys/kernel/pid_max`
- `/proc/sys/kernel/threads-max`
- `/proc/sys/kernel/random/{uuid,boot_id}`
- `/proc/sys/vm/overcommit_memory`
- `/proc/sys/vm/max_map_count`

Los scripts de arranque (`/etc/init.d/*`) y algunos tests leen estos.

### 3.4 `/proc/meminfo` con más campos

Faltan `Active`, `Inactive`, `SwapCached`, `Mapped`, `Committed_AS`.
Algunos scripts de monitorización los leen.

### 3.5 `statfs` para tarfs con bloques reales

Opcional. Ahora `tarfs` reporta 0 bloques y `df -h` lo filtra (como
Linux filtra `/proc`). Si quieres que aparezca sin `-a`, devolver
bloques reales del tar subyacente.

### 3.6 `sysinfo(2)` más completo

Ahora `k_sysinfo` rellena totalram/freeram/procs. `free` (procps)
también llama `sysinfo`. Revisar que loads[] y sharedram son
coherentes.

### 3.7 `uname -a` — `processor`/`hardware-platform`

glibc rellena esos campos copiando `machine` (por eso ves `x86_64`
tres veces). Es cosmético. Dejar como está.

### 3.8 Señales reales

`rt_sigaction`, `rt_sigprocmask` funcionan. Falta:
- `signalfd` (usa `SIG_SETMASK` + `read`).
- `sigaltstack` con `SA_ONSTACK`.
- `pidfd_send_signal`.

Los usa `bash` para job control más fino.

### 3.9 `epoll`

`epoll_create1` / `epoll_ctl` / `epoll_wait`. Sin ellos:
- `tmux`, `screen` no van.
- `python3` con asyncio no va.
- Algunos tests de red fallarán cuando haya red.

Es un proyecto de una tarde. Se puede implementar sobre la infra de
`wait_queue_t` y `poll`.

### 3.10 `eventfd` / `timerfd`

Los pide `libuv`, `glib`, `systemd` (que no corre aquí, pero los
scripts sí). `eventfd` es ~50 líneas.

### 3.11 `signalfd`

`signalfd4`. `bash` no lo usa, pero `make -j`, `xargs -P` y otros
podrían. Media hora.

---

## 4. Objetivos medios (una semana de trabajo)

### 4.1 Red: loopback + stack IP mínimo

El objetivo más transformador después de tener userspace estable.

- **Fase 1**: loopback (`lo`). Interfaces `socket/bind/connect/send/recv`
  sobre `AF_INET` con `127.0.0.1`.
- **Fase 2**: driver de red. QEMU con `-netdev user` expone `e1000` o
  `virtio-net`. Empezar por `e1000` (más simple).
- **Fase 3**: ARP, ICMP, UDP.
- **Fase 4**: TCP mínimo (handshake + data + close).

**Desbloquea**: `ping`, `curl`, `wget`, `nc`, `ssh` cliente, `git clone`
por http, DNS.

### 4.2 `vDSO`

Ahora `clock_gettime` es syscall real (~150 ns). Con vDSO es lectura
de memoria (~5 ns). Programas con bucles tight (`date`, benchmarks,
`perf`) lo notan.

**Implementación**:
- Generar un ELF pequeño en build time con stubs que hacen `rdtsc` +
  conversión.
- Mapearlo en cada proceso a una VA fija.
- Publicar `AT_SYSINFO_EHDR` en el auxv.

Es un proyecto de 1-2 días. No urgente, pero `python3` y `perf` se
benefician.

### 4.3 `clone3` y `rseq` de verdad

Ahora `clone3` cae a `clone` y `rseq` devuelve `-ENOSYS`. glibc lo
acepta, pero:
- `pthread_create` es un pelín más lento.
- `sched_getcpu()` en tight loops da un aviso.
- `python3` con threading puede portarse mejor.

**`clone3`**: syscall 435. Es una versión extendida de `clone` con
struct `clone_args`. ~100 líneas.

**`rseq`**: syscall 334. Registrar el área en `task_t`, actualizar
`cpu_id` al hacer context switch. ~150 líneas.

### 4.4 Usuarios reales: setuid, `getpwnam`, `/etc/passwd` completo

Hoy todo corre como root. Añadir:
- `setuid` real (bajar privilegios de forma irreversible).
- `getpwnam`/`getpwuid` leyendo `/etc/passwd` con NSS.
- `/etc/shadow` para `login`, `su`, `passwd`.

**Desbloquea**: `sudo`, `su`, `login`, `sshd`, políticas de seguridad.

### 4.5 `ptrace` mínimo

`strace` en el guest sería una revolución para debug. `ptrace` es
grande (~500 líneas), pero se puede hacer un subset:
- `PTRACE_ATTACH` / `PTRACE_DETACH`.
- `PTRACE_SYSCALL` (parar al entrar/salir de syscall).
- `PTRACE_PEEKDATA` / `PTRACE_POKEDATA`.
- `PTRACE_GETREGS` / `PTRACE_SETREGS`.

Esto desbloquea también `gdb` en el guest, que es enorme.

---

## 5. Objetivos grandes (un mes+)

### 5.1 Portar `python3`

El salto cualitativo. `python3.12` de Ubuntu pide:
- libs: ~40 (`libpython3.12.so.1.0`, `libexpat`, `libssl`,
  `libcrypto`, `libffi`, `liblzma`, `libbz2`, `libsqlite3`, ...).
- syscalls: `epoll_create1`, `eventfd2`, `clone3`, `rseq`, `signalfd4`,
  `memfd_create`, `statx`, `getrandom` con `GRND_NONBLOCK`, `futex`
  con `FUTEX_WAIT_BITSET` y `FUTEX_WAKE_BITSET`.
- `/proc/self/maps` con paths reales.
- `mmap` con `MAP_FIXED_NOREPLACE`, `MAP_STACK`, `MAP_DENYWRITE`.

Es alcanzable **después** de red (5.1 sin networking es trabajo a
medias) y vDSO.

### 5.2 Portar `git`

`git` sin red es utilísimo:
- `git init`, `git add`, `git commit`, `git log`, `git diff`, `git clone file://`.
- Necesita: `libz`, `libcurl` (opcional), `perl` (para algunos scripts),
  `getpwnam`, `/etc/passwd` con `user.name`/`user.email` de config.

Con red (5.1) desbloquea clonar por http/https.

### 5.3 Portar `gcc` nativo

El sueño. Es enorme:
- Binutils (`as`, `ld`).
- `libgcc`, `libstdc++`.
- `cc1` (frontend C).
- Sysroot con `/usr/include` completo, `/lib/crt*.o`, `libc.so`.

Alternativa más realista: **`tcc`** ya funciona. Añadirle:
- `-static` con `libtcc1.a` de musl o glibc.
- `tcc -run` para C complejo (más de un fichero).

### 5.4 Soporte de editores gráficos

`vim` en modo consola funciona si `libtinfo` va. `emacs` es un
proyecto entero aparte. `code` no aplica.

**Objetivo realista**: `vim` funcional, `nvim` si se puede.

### 5.5 Servidor gráfico tipo Wayland o X11

No aplica a corto plazo. Tu compositor propio es el camino correcto
para Aurora. Lo de glibc es para binarios CLI. Portar GTK/Qt
requeriría:
- Wayland o X11.
- DRM (`/dev/dri/card0`, `ioctl` DRM).
- EGL/GL.
- Mesa.

Es un proyecto de meses.

### 5.6 Networking avanzado

Una vez con TCP:
- `sshd` (sshd de OpenSSH usa `epoll`, `pty`, `signalfd`, `clone`
  con namespaces).
- `curl` con TLS (`libssl` portado).
- `apt` (necesita `dpkg`, `tar`, `gpg`, más red).

---

## 6. Deuda técnica y refactor pendiente

### 6.1 Logs de diagnóstico que quedan en el código

Los que quedaron sin limpiar tras los fixes de bash:
- `syscall.c`: bloque `[DF-SYS]`, `[DF-OPEN]`, `[DF-STATFS]`,
  `[BASH-SYS]`, `[BASH-READ-DIAG]`, `[BASH-STDERR]`, `[IOCTL-DIAG]`,
  `[GETTIMEOFDAY-DIAG]`, `[KLOGCTL]`.
- `pf.c`: `[DF-PF]`, `[STACK-GROW-DIAG]`, `[STACK-DIAG]`,
  `[MMAP-ANON]`.
- `tty.c`: `[TTY] TCGETS` one-shot.
- `vfs.c`: `[PROCFS-OPEN]` (a TRACE, aceptable).

**Acción**: buscar por `"[DF"`, `"[BASH"`, `"[STACK"`, `"[MMAP-ANON"`
y quitar. Mantener los que disparan en error (`SYSCALL-ERR`,
`SYSCALL-GAP`, `OPEN-FAIL`, `uaccess fixup`, `[PF] Matando`).

### 6.2 Mecanismo de device IDs (`fs_dev_id`) es de 16 slots

Si alguien monta más de 16 FS, los últimos colapsan al device
`32 + FS_DEV_MAX`. Aceptable para ahora. A futuro:
- Aumentar `FS_DEV_MAX` a 64.
- O asignar el device en `vfs_mount` y guardarlo en la tabla de mounts.

### 6.3 `p->name` vs `exe_path`

Hoy `p->name` es el basename del path (por `set_proc_name_from_path`).
Lo usamos para `comm` y `exe`. Añadir un `exe_path[VFS_PATH_MAX]` con
el path completo es trabajo pequeño y limpia varias rutas de procfs.

### 6.4 Syscall `statx`

Algunos binarios modernos (coreutils 9.x) llaman `statx` (332) primero
y caen a `fstatat` si falla. Añadir `statx` es ~30 líneas y reduce
dos syscalls por cada `stat`.

### 6.5 `/etc/ld.so.cache` no existe → búsqueda lineal de libs

Cada `execve` con glibc hace ~8 `openat` por lib antes de encontrarla.
Con un `/etc/ld.so.cache` real, glibc lo lee una vez y va directo.
Es cuestión de:
- Generar el cache en el host con `ldconfig -r sysroot`.
- Copiarlo al initrd.

Ahorro: ~50 ms por exec de binario grande.

---

## 7. Backlog ordenado por prioridad

| # | Item | Esfuerzo | Prioridad |
|---|---|---|---|
| 1 | `/proc/vmstat` (para `vmstat`) | 5 min | Alta |
| 2 | Arreglar `pgrep` | 30 min | Alta |
| 3 | `exe_path` en `process_t` + `/proc/<pid>/exe` real | 1 h | Alta |
| 4 | Verificar `less`/`nano`/`ed` interactivamente | 15 min | Alta |
| 5 | Limpiar logs de diagnóstico | 30 min | Media |
| 6 | `statx` | 1 h | Media |
| 7 | `epoll` + `eventfd` | 4 h | Media |
| 8 | `/etc/ld.so.cache` en el initrd | 1 h | Media |
| 9 | `signalfd` | 1 h | Media |
| 10 | Usuarios reales (`setuid`, `getpwnam`) | 1 día | Media |
| 11 | Probar `perl` y `python3` | 2 h | Alta |
| 12 | Portar `git` | 1 semana | Media |
| 13 | Red loopback | 2 días | Alta |
| 14 | Driver de red (`e1000`) | 3 días | Alta |
| 15 | TCP funcional | 1 semana | Alta |
| 16 | vDSO | 2 días | Baja |
| 17 | `clone3` + `rseq` | 1 día | Baja |
| 18 | `ptrace` (subset) | 3 días | Baja |
| 19 | Portar `gcc` | 1 mes+ | Baja |

---

## 8. Cómo probar tras cada cambio

Cada vez que toques `syscall.c`, `procfs.c`, `pf.c` o `vfs.c`, la
forma barata de verificar es:

```bash
make -C kernel 2>&1 | grep -iE "error|warning: implicit" | head
make image
make run-smp-kvm-ahci
Y en el guest, un mini-smoke:

text
/usr/bin/df -h
/usr/bin/df -h -a
/usr/bin/stat -f /data
/usr/bin/ls -la /
/usr/bin/cat /proc/self/mountinfo
/usr/bin/free
/usr/bin/uptime
/usr/bin/du -sh /etc
Si esos 8 van bien, la mayoría del userspace sigue sano.

Para comprobar regresiones específicas:

text
/usr/bin/bash /smoke.sh
(si lo integras en el initrd, ver sección 9).

9. Tooling pendiente
9.1 Integrar scripts/smoke.sh en el Makefile
Añadir al initrd.tar:

makefile
	@[ -f scripts/smoke.sh ] && cp -f scripts/smoke.sh sysroot/smoke.sh \
		&& chmod +x sysroot/smoke.sh || true
Y en el guest:

text
/usr/bin/bash /smoke.sh
Salida: un resumen por bloques con OK/FAIL/SEGV/HANG.

9.2 Tabla de símbolos del kernel
Añadir un kernel.syms en .rodata con (address, name) para cada
función. El panic handler lo lee y resuelve el backtrace a
funcion+offset en lugar de solo direcciones.

Extraer con nm -n kernel.elf | grep " T " en build time.

Embeber con objcopy -I binary.

En backtrace(), buscar la entrada más cercana.

Ahorra horas de objdump/nm cada vez que hay un panic.

9.3 dmesg en el guest ya funciona
dmesg | tail -30 lee el ring de klog. Útil para reproducir sin
sacarle el serial al host.

10. Referencias útiles
Linux ABI x86_64: https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl

glibc sysdeps: https://sourceware.org/git/?p=glibc.git;a=tree;f=sysdeps/unix/sysv/linux;hb=HEAD

man pages: man 2 <syscall> para cada uno.

Testing: tests/ en tu repo, con kernel_tests.c como patrón.

Notas
Este documento cubre solo el port de userspace glibc. Otros
subsistemas de Aurora (compositor, red propia, drivers) tienen su
propio backlog.

La lista de "Pendientes inmediatos" (§2) es la que debería
priorizarse en las próximas sesiones.

Los números de esfuerzo son estimaciones aproximadas; el tamaño real
depende del estado del kernel en cada momento.