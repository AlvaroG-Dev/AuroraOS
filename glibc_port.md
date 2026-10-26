# AuroraOS — Plan de compatibilidad Linux/glibc

Última revisión: 2026-10-15. Este documento refleja el estado descrito por el código y la documentación actuales del repositorio. Los checks end-to-end indicados son los que constan en el proyecto; esta revisión remota no ejecuta el kernel ni los tests.

## 1. Alcance y criterio de éxito

El objetivo no es portar glibc al kernel: AuroraOS proporciona una ABI Linux x86-64 suficiente para ejecutar binarios Linux dinámicos enlazados con glibc, además de los binarios musl ya soportados. La compatibilidad se amplía por aplicaciones reales y regresiones reproducibles, no por contar syscalls implementadas.

**Estado global:** compatibilidad avanzada y útil en userspace, pero todavía parcial. Bash interactivo, coreutils, utilidades de texto/compresión, editores de terminal, Perl 5.38 dinámico, Python 3.12 y Git 2.43 figuran como operativos en el entorno del proyecto. No equivale a compatibilidad completa con una distribución Linux.

## 2. Capacidades que ya constan como funcionales

- **Carga dinámica ELF/glibc:** intérprete dinámico, bibliotecas compartidas y `/etc/ld.so.cache` generado en build time.
- **Shell y herramientas:** bash interactivo; coreutils, grep/sed/awk y utilidades de compresión; `less`, `nano`, `ed`; `tcc -run`.
- **Procesos y threads:** `clone3` con fallback a `clone`; futex WAIT_BITSET/WAKE_BITSET; Python threading probado con 20 threads + join.
- **Multiplexación y eventos:** `epoll_create1/ctl/wait`, `epoll_pwait/pwait2`, `eventfd/eventfd2` y `signalfd/signalfd4` están implementados. La propagación de wakeups usa wait queues, no polling deliberado.
- **Procfs y memoria:** `/proc/<pid>/stat` (52 campos), `status` extendido, `maps/smaps`, `mountinfo`, `vmstat`, `statx`, `vDSO`, `madvise(DONTNEED/FREE)` y nodos `/dev/random`/ `/dev/urandom`.
- **Filesystem y herramientas reales:** Git 2.43 ejecuta init/add/commit/log/status/diff en FAT32 RW; Perl 5.38 dinámico y Python 3.12 con stdlib y extensiones C constan como operativos.

Estas capacidades deben conservarse mediante pruebas de regresión; no se deben volver a presentar como tareas pendientes sin evidencia de una regresión.

## 3. Backlog priorizado y actualizado

### P0 — Corregir discrepancias y asegurar la base

- [ ] **Reconciliar documentación duplicada**: `TODO.md`, `ROADMAP.md` y este archivo contienen estados antiguos. Marcar explícitamente epoll/eventfd/signalfd, Python, Perl, Git, vDSO y `/proc` extendido como cerrados donde corresponda.
- [ ] **Añadir una smoke suite reproducible** al build/CI: arranque, bash interactivo, `ls -la`, `statx`, `df -h`, procfs, PTY, señales, threads/futex, epoll/eventfd/signalfd y Git sobre FAT32 RW. Guardar logs y códigos de salida.
- [ ] **Auditar syscalls y errores por aplicación**, usando `[SYSCALL-GAP]`, `[SYSCALL-ERR]` y `[OPEN-FAIL]`; clasificar cada fallo como syscall ausente, semántica ABI incorrecta, filesystem o userspace. No implementar syscalls solo porque aparecen en logs: reproducir primero.

### P1 — Concurrencia, señales y semántica POSIX/Linux

- [ ] **Señales:** comprobar el filtrado de señales ignoradas en `signal_check_pending` y `wait_common`; revisar `sigaltstack` + `SA_ONSTACK` y `pidfd_send_signal` si las aplicaciones objetivo lo requieren. Retirar `[SIG-DBG]` solo tras cerrar y probar esa deuda.
- [ ] **Futex/threads:** stress tests concurrentes de WAIT/WAKE, WAIT_BITSET, WAKE_BITSET, salida de threads, señales y cancelación. `clone3` no es aún semánticamente completo: el fallback rechaza flags no soportados (por ejemplo PIDFD/CGROUP) con `EINVAL`; `rseq` sigue pendiente según el estado documentado.
- [~] **epoll:** `EPOLLET`, `EPOLLONESHOT` y `EPOLLEXCLUSIVE` implementados en `kernel/epoll.c` (2026-10-24; ET por contador de despertares por entrada, EXCLUSIVE validado como en Linux pero sin despertar selectivo; ERR/HUP se reportan siempre). Test de kernel `wait: subscribe_seq por entrada`. Pendiente: test de userspace (ET/ONESHOT con pipe/eventfd) y `EPOLLRDHUP` real.
- [ ] **PTY/TTY:** investigar `ioctl(TCGETS)` sobre PTY, que Python registra como `ENOTTY` al cargar `encodings/utf_8`; crear pruebas de ioctl, termios, foreground process group y cierre de PTY.

### P2 — Interfaces de sistema y usuarios

- [x] `/proc/meminfo` extendido: el TODO registra que se completó en el bloque 12. No volver a tratarlo como pendiente sin una regresión concreta.
- [x] Syscalls de identidad implementadas en `kernel/syscall.c`: `setuid/setgid`, `setreuid/setregid`, `setresuid/setresgid` y getters relacionados. La semántica debe mantenerse con tests de permisos/credenciales, pero no es una función ausente.
- [ ] **Validar, no presuponer, la semántica de identidad en ejecución:** pruebas de transiciones root/no-root, `execve`, `setuid` irreversible y comprobaciones VFS de uid/gid. Abrir un bug solo si una prueba falla.
- [ ] **NSS/glibc userspace:** `getpwnam/getpwuid/getgrnam` son interfaces de libc/NSS, no syscalls del kernel. Probarlas con los módulos/archivos disponibles en el sysroot; clasificarlas como problema de userspace si fallan.
- [ ] Revisar coherencia de `/proc/self/mounts` y `mountinfo`, y `sysinfo(2)` solo si las aplicaciones objetivo muestran discrepancias reproducibles.
- [ ] Revisar `statfs` por filesystem con pruebas de `df`/`stat -f`; no convertirlo en tarea genérica sin un caso fallido reproducible.

### P3 — Aplicaciones y funciones de mayor alcance

- [ ] **Networking:** loopback y stack IP, driver NIC estable y TCP funcional. Esto desbloquea `git push/pull`; no es un problema específico de glibc, pero sí una limitación práctica de las aplicaciones.
- [ ] **`ptrace` mínimo** si se quiere soportar depuración con GDB.
- [ ] **GCC nativo** y herramientas de desarrollo completas: objetivo grande, después de estabilizar ABI, filesystem y red.
- [ ] Acelerar el arranque de Python compilando `.pyc` durante el build si las mediciones lo justifican.
- [ ] No ampliar a compatibilidad genérica con distros hasta tener una suite de conformidad y una lista concreta de aplicaciones objetivo.

## 4. Detalles que no deben confundirse

- `epoll`, `eventfd` y `signalfd` están implementados; sus flags/semánticas avanzadas siguen siendo deuda.
- `clone3` con fallback a `clone` no significa que todos los flags de `clone3` estén soportados.
- `rseq` sigue pendiente según el propio documento previo.
- Git funciona localmente sobre FAT32; `push/pull` depende de la red.
- El soporte de Python/Perl se refiere a las versiones y pruebas registradas, no a toda extensión o paquete de terceros.
- Los errores `ENOSYS`/SYSCALL-GAP deben priorizarse según una aplicación y una reproducción concreta, no por número bruto.

## 5. Pruebas mínimas para aceptar un cambio

1. Compilar kernel y comprobar errores/avisos relevantes.
2. Crear la imagen y arrancar con la configuración habitual de pruebas (`make run-smp-kvm-ahci` si sigue disponible).
3. Ejecutar la smoke suite y comparar códigos de salida/logs con la versión base.
4. Ejecutar pruebas específicas del subsistema modificado y un test de concurrencia si toca locks, wait queues, señales, memoria o I/O.
5. Registrar el cambio y su resultado aquí y en `TODO.md`/ `ROADMAP.md` para que el estado no vuelva a divergir.

Comandos de comprobación manual ya usados por el proyecto:

```sh
make -C kernel 2>&1 | grep -iE "error|warning: implicit" | head
make image
make run-smp-kvm-ahci
/usr/bin/df -h
/usr/bin/df -h -a
/usr/bin/stat -f /data
/usr/bin/ls -la /
/usr/bin/cat /proc/self/mountinfo
/usr/bin/free
/usr/bin/uptime
/usr/bin/du -sh /etc
/usr/bin/bash /smoke.sh
```

## 6. Referencias del repositorio

- `kernel/syscall.c`: dispatcher Linux x86-64 y tabla de handlers.
- `kernel/epoll.c` / `kernel/epoll.h`: epoll y eventfd.
- `kernel/glibc_port.md`: este plan de seguimiento.
- `TODO.md` y `ROADMAP.md`: actualizar en conjunto cuando cambie el estado.
- ABI de referencia: tabla de syscalls Linux x86-64 y documentación de cada syscall.

**Próximo bloque recomendado:** P0, empezando por smoke tests automatizados y reconciliación de estados; después corregir las semánticas que aceptan flags de epoll sin implementarlos. Es la forma más segura de avanzar sin romper los programas que ya funcionan.

## 7. ABI Linux añadido para el stack gráfico (Wayland/GTK/Qt/Mesa)

- AF_UNIX completo (STREAM/DGRAM/SEQPACKET, SCM_RIGHTS, SO_PEERCRED, abstractos), `memfd_create` con
  `mmap(MAP_SHARED)` real, `timerfd`, `pidfd_open`, `inotify` (stub), `fallocate`, `creat`,
  `ftruncate`/`truncate`, máscaras de señales con `sig-1` como Linux, `sched_getaffinity` con las CPUs online.
- devtmpfs + `mknod` de char devices, `/dev/dri/{card0,renderD128}`, `/dev/input/event*`, `/dev/tty0`,
  sysfs dinámico (`/sys/class/{input,drm}`), udev data mínima y `AF_NETLINK` uevent.
- `rseq` sigue en `-ENOSYS` (glibc lo tolera). Pendiente: `rt_sigsuspend`, `rt_sigpending`,
  `rt_sigtimedwait`, SIGPIPE real en sockets, inodos `S_IFSOCK` en el VFS.
- Rendimiento: en QEMU sin KVM (TCG) los fallos de página que reservan bloques grandes hacen lentas las apps con
  mucha memoria (GTK widget-factory no llega a pintar en 7 minutos); no se ha medido con KVM.
