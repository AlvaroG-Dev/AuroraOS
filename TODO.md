Bloque 1 — Cerrar job control (2-3 sesiones)
Sin esto, Ctrl+C funciona pero Ctrl+Z, fg, bg, jobs y & no.

1.1 TASK_STOPPED en el scheduler (~2 h)
sched.h: añadir TASK_STOPPED al enum task_state_t.

sched.c: sched_tick no elige tareas STOPPED. sched_make_ready las ignora (solo SIGCONT las saca).

Nuevo helper sched_stop_task(t) / sched_cont_task(t).

1.2 SIGTSTP/SIGCONT reales (~3 h)
signal.c: signal_default_action(SIGTSTP/SIGTTIN/SIGTTOU) → STOP real.

Al recibir SIGTSTP: proc->state = TASK_STOPPED, quitar de la runqueue, dejar task->on_cpu=0, sched_yield().

Al recibir SIGCONT: si estaba STOPPED, hacerla READY y despertar a su padre con un flag child_continued.

1.3 waitpid con WUNTRACED/WCONTINUED (~2 h)
process_waitpid acepta los flags.

process_t gana was_stopped y was_continued.

La wq del padre se despierta también cuando el hijo cambia de estado stopped/continued.

k_wait4 los traduce al status Linux: ((sig) << 8) | 0x7f para stopped, 0xffff para continued.

1.4 NOFLSH (~30 min)
En tty_slave_receive, cuando llega VINTR/VQUIT/VSUSP y ISIG está activo, hacer canon_len=0 y count=0 antes de emitir la señal.

Solo si NOFLSH no está en lflag. Como no exponemos NOFLSH a userland, se aplica siempre.

1.5 Tests (~2 h)
sched: TASK_STOPPED no elegida por el scheduler

signal: SIGTSTP para el proceso

signal: SIGCONT lo reanuda

waitpid: WUNTRACED detecta stop

tty: NOFLSH limpia canon_buf en Ctrl+C

Cierre: sh -c 'sleep 100' &, jobs, fg, bg, Ctrl+Z, kill %1. Y vi/top pueden instalarse sin colgarse.

Bloque 2 — Boot configurable y pivot (~2-3 sesiones)
Cierra el último [~] del ROADMAP original.

2.1 /etc/aurora.conf (~2 h)
Bootloader lee /etc/aurora.conf de FAT32 antes de cargar el kernel.

Formato key=value. Clave inicial: kernel=/boot/kernel.elf.

Fallback a la ruta hardcoded actual si no existe.

2.2 pivot_root / switch_root (~4 h)
Syscall SYS_PIVOT_ROOT (Linux 155).

Mover la raíz del VFS: new_root pasa a ser /, el antiguo / se mueve a /oldroot.

El kernel arranca con FAT32 en /, monta tarfs en /initrd, hace pivot_root("/initrd", "/oldroot"), y luego umount /oldroot.

Resultado: / es tarfs (RO), /data es FAT32 (RW), sin /initrd visible.

2.3 Layout final (~1 h)
text
/         tarfs (RO)     ← sistema, /bin, /usr, /apps, /system
/data     FAT32 (RW)     ← datos de usuario
/dev      devfs
/proc     procfs (bloque 3.3)
El shell arranca en / (RO). cd /data para escribir.

Sin /initrd. Sin bind mounts.

2.4 Script de rescate (~1 h)
Un shell script /sbin/aurora-fsck para verificar FAT32 tras un crash.

Detección de "unclean shutdown" al montar: si FAT32 no está limpio, correr fsck mínimo.

Cierre: el sistema tiene la estructura de un Linux real.

Bloque 3 — Completitud VFS/FS (~3-4 sesiones)
3.1 Symlinks (~2 h)
SYS_SYMLINK (88), SYS_READLINK (89, hoy stub).

vfs_lookup sigue symlinks del tarfs (ya funciona) y de FAT32 (no soportado por FAT).

line_t... no, perdón, esto es kernel.

vfs_ops_t gana .symlink y .readlink.

Tarfs ya emite typeflag='2', solo falta exponerlo a userland.

3.2 Hard links (~1 h)
SYS_LINK (86).

FAT32 no los soporta: -EPERM salvo en tarfs (RO, no tiene sentido crear).

3.3 /proc mínimo (~1 día)
procfs.c: árbol dinámico generado on-demand.

/proc/self/ → symlink a /proc/<pid>/.

/proc/<pid>/{stat,status,cmdline,exe,fds/}.

/proc/mounts, /proc/uptime, /proc/meminfo.

Sin esto, ps, top, free no funcionan.

3.4 Permisos y metadatos (~3 h)
chmod, chown reales sobre process_t->uid/gid (ya son 0, extenderlos).

umask por proceso.

S_ISUID/S_ISGID (setuid/setgid al execve).

3.5 mmap file-backed / VMA_FILE (~1 día)
sys_mmap con fd != -1 crea un VMA de tipo VMA_FILE.

El demand pager lee del inodo al tocar página.

Desbloquea dlopen, mmap(PROT_EXEC) de binarios, mmap de libc.

3.6 Buffer cache FAT32 (~1 día)
Cache de clusters en RAM (LRU).

Reduce I/O en operaciones repetidas (grep -r, find, etc.).

3.7 Tests de corrupción FAT32 (~2 h)
Fuzzing del BPB parser con datos aleatorios.

Detección de ciclos en la cadena FAT.

Manejo de cluster out-of-range.

Simular "apagado durante escritura" y verificar recuperación.

Cierre: el FS se comporta como un FS POSIX completo (salvo symlinks en FAT32, que es imposible).

Bloque 4 — Syscalls de relleno (~1-2 sesiones)
4.1 statfs/fstatfs (~1 h)
Devuelve f_blocks, f_bfree, f_bavail, f_bsize.

Lo usa df, stat -f.

4.2 utimensat, fsync, fchmod, fchown (~2 h)
utimensat (280): timestamps. FAT32 tiene 2 s de granularidad.

fsync (74): flush del FS (FAT32 ya lo tiene, exponerlo).

fchmod/fchown: -EPERM en FAT32, no-op en tarfs.

4.3 mprotect real (~2 h)
Recorrer los VMAs y actualizar PTEs.

Lo usan dlopen, mprotect(PROT_NONE) para guard pages.

4.4 mknod real (~1 h)
Crear nodos char/block en devfs.

Lo usa mdev si algún día añades hotplug.

4.5 getrlimit/setrlimit (~1 h)
prlimit64 (302) hoy devuelve -ENOSYS.

Implementar con valores por defecto (RLIMIT_NOFILE=256, RLIMIT_STACK=8MB).

4.6 Syscalls varias (~2 h)
sethostname, setdomainname.

getrusage (98).

times (100).

Cierre: busybox df, stat, ulimit completos.

Bloque 5 — Userland / SO (~2-3 sesiones)
5.1 /etc/passwd, /etc/group (~1 h)
Ficheros en el tarfs.

Silencia whoami, id, ls -l (que hoy dicen "uid 0" pero sin nombre).

5.2 init (PID 1) (~3 h)
Un binario /sbin/init que:

Monta /proc, /sys, /tmp.

Lanza getty/terminal en /dev/tty1.

Reparenta huérfanos (reaper).

Reap de zombies sin padre.

Hoy el shell es directamente PID 1, sin reaper.

5.3 Reaper de zombies sin padre (~2 h)
En process_exit, si el padre ya ha muerto, el proceso pasa a ser hijo de init.

init recoge zombies automáticamente (waitpid(-1) en loop).

5.4 getty minimal (~2 h)
Abre el terminal, hace login (o directo sh sin password).

Alternativa: -noshell en /etc/aurora.conf para arrancar sin login.

Cierre: el sistema tiene un init real. Los huérfanos no se acumulan.

Bloque 6 — Mejoras de input/terminal (~1-2 sesiones)
6.1 Scancodes extendidos (~2 h)
0xE0 0x48 → flecha arriba → emitir \e[A al TTY.

Lo mismo para F1..F12, Page Up/Down, Home/End.

vi, top, less usan estas secuencias.

6.2 Copy/paste (~1 día, opcional)
Modo "selección" en el terminal app (Shift+flechas o clic+drag).

Buffer interno en la app (no kernel).

6.3 Cursor parpadeante (~1 h)
El terminal app alterna el cursor cada 500 ms.

Requiere redibujar la fila del cursor en cada tick.

6.4 Serial como consola interactiva (~2 h, opcional)
Redirigir input del UART al PTY y viceversa.

Útil para -nographic en QEMU.