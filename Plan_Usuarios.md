# Plan de implementación: usuarios, grupos y permisos en AuroraOS (nativo + compatibilidad Linux)

Estado (1-nov-2026): **U0 y U1 hechas** (cred_test: 31 pruebas OK; U1.5 chown fino y tkill/tgkill incluidos; falta solo `nosuid` en montajes, que se hará con U2) en la rama `claude/plan-usuarios`. `user/apps/cred_test` (25 pruebas) pasa en QEMU: `fsuid` en el VFS y `access()` con uid real (U1.1), sticky en `/tmp` y `/dev/shm` + permisos en `rename` + propietario de lo creado en tmpfs = `fsuid` (U1.2), permisos de `kill(2)` por uid (U1.3, falta `tkill/tgkill`), `AT_UID/EUID/GID/EGID/AT_SECURE` reales y `PR_SET/GET_NO_NEW_PRIVS` (U1.4, el `nosuid` de montajes queda pendiente), setuid en `execve` al estilo Linux (el uid real no cambia; suid=euid). **U2.1 hecha** (modo/uid/gid de FAT32 persisten en `/data/.aurora-meta`, con CRC y copia `.aurora-meta.new`; `sync(2)` ahora vuelca FAT32 y la tabla; prueba `scripts/meta-test.sh`: dos arranques con el mismo disco, 8/8 OK). **U2.3 hecha** (`passwd`/`group`/`shadow` se cargan desde `/data/etc` al montar `/data` y se guardan en `sync`/apagado; `shadow` es 0600; `meta-test.sh` 12/12 OK). `nosuid` hecho (execve ignora setuid/setgid en volúmenes FAT32); U2.2 (opciones de montaje uid/gid/fmask) queda diferida. **U3 hecha** con herramientas propias sobre glibc, sin busybox: `user/aurora-users/aurora-users.c` (un binario multicall: `login`, `su`, `passwd`, `useradd`, `sudo`, con alias `aurora-*`), compilado por `scripts/build-users.sh` contra la glibc del host, `su/passwd/sudo/login` en modo 4755; SHA-512 con `crypt()`; `/etc/sudo-nopasswd` hace que `sudo` no pida clave a `wheel` en el LiveUSB; `alvaro` ahora está en `wheel`. Prueba `scripts/users-test.sh` en QEMU: 16/16. Siguiente: U4 (`init → getty → login`). Se compila con `make iso WITH_NVIDIA=0`; ejecutar la prueba: `WITH_NVIDIA=0 scripts/qemu-boot-test.sh /usr/bin/cred_test`.
Parte de `main` (a71d9c57).
Contexto: `ROADMAP.md` marca «Usuarios y grupos» como hecho, pero eso es **solo la mitad kernel**. Este plan cubre lo que falta para que haya usuarios *de verdad* (login, contraseñas, privilegios mínimos, sesión gráfica por usuario).

## 0. Principios

1. **Semántica idéntica a Linux** para lo que ve el espacio de usuario (**glibc**, Weston, Mesa): mismos números de syscall, mismos errno, mismos formatos de `/etc/passwd`, `/etc/shadow`, `/etc/group`. No inventar una API propia de usuarios. **La libc de Aurora es glibc.**
2. **Primero lo verificable en QEMU**, hito a hito; el hardware real (Portégé, 12700KF) solo para confirmar al final.
3. **No romper el arranque ni el LiveUSB.** Hoy todo corre como root; el paso a «usuario normal» es gradual y reversible con `aurora.conf` / cmdline (`aur.login=off` devuelve el comportamiento actual).
4. **Se sustituye `euid == 0` por comprobaciones de capacidad** (`capable(CAP_X)`) solo cuando el resto esté estable (fase U6). Antes, root sigue siendo «todopoderoso» como en Linux sin caps.
5. **Sin PAM de momento.** NSS «files» de glibc + `/etc/shadow` directo. PAM solo si algún programa portado lo exige (U8, opcional).
6. **Seguridad práctica, no teórica:** el objetivo es que un programa de un usuario normal no pueda tocar el sistema ni a otro usuario; no es un modelo multi-inquilino hostil (sin namespaces ni seccomp en este plan).

## 1. Qué existe ya (verificado en el código, 31-oct-2026)

| Pieza | Estado | Dónde |
|---|---|---|
| Credenciales por proceso: `uid/euid/suid/fsuid`, `gid/egid/sgid/fsgid`, `groups[NGROUPS_MAX]`, `ngroups`, `umask` | sí | `process.h:189-203`, `process.c:554` |
| `getuid/geteuid/getgid/getegid`, `setuid/setgid`, `setreuid/setregid`, `setresuid`, `getgroups/setgroups`, `umask` con reglas de Linux (root cambia todo; el resto solo entre valores que ya tiene) | sí | `syscall.c:1272-1640` |
| `chmod/fchmod/chown/fchown` en el VFS con reglas root/dueño; solo root pone `S_ISUID/S_ISGID` ajenos | sí | `vfs.c:1243-1350`, `syscall.c:4486+` |
| `vfs_check_access`: root salta todo; si no, dueño → grupo (primario + suplementarios) → otros; usado en lookup/create/mkdir/unlink/open/execve/`faccessat` | sí | `vfs.c:1375` |
| setuid/setgid en `execve` | sí | `process.c:2582` |
| `SO_PEERCRED` / `SCM_CREDENTIALS` en sockets UNIX | sí | `unix_socket.c` |
| `/etc/passwd`, `/etc/group`, `/etc/shadow` de ejemplo (root, alvaro, nobody; grupos root/wheel/users/alvaro/nogroup) | sí | `etc/` |
| `nsswitch.conf` con `files`; el Makefile copia `etc/` al sysroot y `tmpfs.c` siembra `/etc` en tmpfs | sí | `Makefile:67-82`, `tmpfs.c:910` |
| `capget` (devuelve todo a cero), `capset` | **stub** | `syscall.c:6117` |
| `PR_SET_NO_NEW_PRIVS` | **ignorado** | `syscall.c:1773` |
| Capacidades de drivers `.aur` (`AUR_CAP_*`) | sí, otro concepto | `aur.h:36+` |

## 2. Qué falta (huecos reales)

1. **Todo arranca como root** (`process.c:554`; `init` y la terminal gráfica). No hay `login`, `getty`, `su`, `sudo`/`sudo`, `passwd`, `adduser`; `shadow` tiene todas las cuentas bloqueadas (`*`).
2. **El almacenamiento no guarda propietarios.** FAT32 (`/data`) pone `uid=gid=0` en cada nodo (`fat32.c:3324`), `chown` es un no-op y el modo se guarda solo en RAM (`fat32_ram_mode_*`), así que se pierde al reiniciar. El raíz es tarfs de solo lectura. Sin esto no hay `/data/home/<usuario>` privado ni persistencia de contraseñas.
3. **`/etc` vive en tmpfs** sembrado desde tarfs: un `passwd` cambiado se pierde al apagar. Además la siembra de reserva (`root:/root`) no coincide con `etc/passwd` (`root:/`).
4. **Detalles del VFS/kernel sin cerrar:** `vfs_check_access` usa `euid`/`egid` y no `fsuid`/`fsgid`; no hay bit *sticky* (`/tmp`); `kill()` y `/proc/<pid>` no comprueban propietario (verificar en U1); sin `nosuid`/`noexec` en montajes; `AT_SECURE` va a 0 incluso tras setuid (`process.c:501`); `no_new_privs` ignorado; `setgroups` no se hereda/inicializa desde `/etc/group` (nadie llama a `initgroups`).
5. **Modelo de privilegios binario** (`euid == 0`): ~20 comprobaciones dispersas (`syscall.c` 12, `vfs.c` 6, `kmod_sys.c`, `process.c`). No hay capabilities: `ping` o un servidor en el puerto 80 exigen ser root entero.
6. **Sesión gráfica:** `aurora-wayland` y la terminal corren como root y abren `/dev/dri/*` y `/dev/input/*` directamente. No hay propiedad de dispositivos por grupo (`video`, `input`, `render`), ni `XDG_RUNTIME_DIR` por usuario, ni «seat».
7. **Herramientas de usuario ausentes** en la ISO: hay que compilar contra glibc **shadow-utils** (`passwd`, `useradd`, `groupadd`, `chpasswd`, `su`, `login`) y **util-linux** (`agetty`/`getty`, `login`, `su`, `runuser`) o, si pesan demasiado, escribir versiones pequeñas propias (`aurora-login`, `aurora-su`, `aurora-passwd`) sobre `getpwnam_r`/`crypt`. `id`, `whoami`, `groups` vienen de coreutils (o de los propios de Aurora).

## 3. Decisiones que necesito de Alvaro (con recomendación)

| # | Decisión | Opciones | Recomendación |
|---|---|---|---|
| **DU1** | Cómo guardar propietario/modo en `/data` (FAT32 no los tiene) | (a) fichero de metadatos *sidecar* (`/data/.aurora-meta`, tabla `cluster inicial → mode/uid/gid`) ; (b) escribir un driver **ext2/ext4** propio y pasar `/data` a eso; (c) UMSDOS-style por fichero | **(a) ahora, (b) después** en un plan aparte. (a) cabe en 1 hito y no obliga a reformatear el USB; (b) es el destino correcto pero es un proyecto grande. |
| **DU2** | Comportamiento del LiveUSB por defecto | (a) autologin como `alvaro` (uid 1000) con `sudo`/`su` sin contraseña para el grupo `wheel`; (b) autologin como root (como hoy); (c) pantalla de login siempre | **(a)**: ya se usa como un usuario normal pero sin fricción en el portátil de pruebas. `aur.login=root` queda de escotilla. |
| **DU3** | Programa para elevar privilegios | `sudo` real (sobre glibc), `su` de shadow-utils/util-linux, o un `aurora-su` propio | **`su` de shadow-utils + `sudo` real** si compila sin PAM (`--without-pam`); si no, `aurora-su` propio (pequeño, setuid-root). |
| **DU4** | Hash de contraseñas | SHA-512 (`$6$`) / yescrypt (por defecto en glibc moderna con `libxcrypt`) | **SHA-512** (`$6$`), universalmente soportado por `libcrypt` de glibc/libxcrypt; yescrypt más adelante. |
| **DU5** | Cambiar de usuario en la sesión gráfica | solo login inicial / cambio rápido de usuario | **Solo login inicial** en este plan; el cambio rápido queda como idea (U7.4). |

Mientras no conteste, el plan avanza con las recomendaciones; ninguna de las fases U0–U2 depende de ellas.

## 4. Fases e hitos

Cada hito acaba con una **prueba concreta en QEMU** (script en `scripts/`, o programa de `user/apps/`) y un commit propio. Estimación: S = una sesión corta, M = una sesión, L = varias.

### U0. Línea base y pruebas (S)

Antes de cambiar nada, fijar qué hace hoy el kernel y que los tests lo vigilen.

- **U0.1** Programa `user/apps/cred_test`: lee y cambia credenciales, y comprueba las reglas de §1 (setuid desde root, desde no-root, `setresuid`, `setgroups`, `umask`). Se ejecuta como root y *tras bajarse a uid 1000* (`setgid(1000); setuid(1000)`).
- **U0.2** Programa `perm_test`: crea ficheros/directorios con modos variados en tmpfs y comprueba, como uid 1000, qué se puede leer/escribir/ejecutar/borrar. Cubre `access()`, `faccessat()`, `chmod`/`chown` ajenos, setuid en `execve`.
- **U0.3** Añadirlos a la suite que ya corre `tests_thread` y a `scripts/check_boot.sh`.
- **Hito U0:** `cred_test` y `perm_test` pasan en QEMU y fijan el comportamiento. Los fallos que salgan (esperables: sticky, `fsuid`) se anotan para U1.

### U1. Núcleo de credenciales y permisos correcto (M)

Dejar la parte kernel completa y fiel a Linux, aún con root omnipotente.

- **U1.1** `vfs_check_access` usa `fsuid/fsgid` (y `access()`/`faccessat()` usan uid/gid *reales*, como Linux). Implementar `setfsuid/setfsgid`.
- **U1.2** Bit **sticky** (`S_ISVTX`): en `unlink/rmdir/rename` de un directorio con sticky, solo dueño del fichero, dueño del directorio o root. `/tmp` pasa a `1777`.
- **U1.3** `kill()`/`tgkill`/`sigqueue`: un proceso no-root solo señala a procesos con su uid real/efectivo (regla de Linux, con excepción de `SIGCONT` en la misma sesión). `setpriority`/`sched_setscheduler` análogo. `/proc/<pid>/*` sensible (`environ`, `mem`, `fd`) solo para el dueño o root.
- **U1.4** `execve`: `AT_SECURE=1` cuando hay setuid/setgid o cambia el uid efectivo; al ejecutar setuid se limpian `LD_*` (lo hace libc con `AT_SECURE`); se respeta `PR_SET_NO_NEW_PRIVS` (ya no ignorado) y los montajes `nosuid`.
- **U1.5** Semántica fina de `chown`: dueño no-root no puede regalar el fichero; al cambiar dueño/grupo se borran `S_ISUID/S_ISGID` (regla de Linux); `chmod` de un no-root sobre un fichero de un grupo ajeno quita `S_ISGID`.
- **U1.6** Herencia: `fork/clone` copian todas las credenciales y `groups[]` (comprobar); `setgroups` solo root; `getgroups` con `size==0`.
- **U1.7** `/proc/<pid>/status` con `Uid:`/`Gid:`/`Groups:` completos (cuatro valores cada uno) y `/proc/self/loginuid` simple.
- **Hito U1:** `cred_test`/`perm_test` ampliados pasan (sticky en `/tmp`, kill ajeno → `EPERM`, `AT_SECURE`, `no_new_privs`, `fsuid`).
- **Riesgo:** endurecer permisos puede romper programas que hoy «funcionan» porque todo es root. Mitigación: U1 se prueba aún con todo arrancando como root; el recorte de privilegios llega en U4.

### U2. Persistencia de metadatos y `/etc` (M–L)

Sin esto, los usuarios no sobreviven a un reinicio.

- **U2.1** *Sidecar* de metadatos para FAT32 (DU1-a): tabla `(cluster_inicial → mode, uid, gid)` en `/data/.aurora-meta`, cargada al montar, escrita con *journal mínimo* (escritura a fichero temporal + renombrado) y volcada en `sync`/apagado (`power.c` ya hace `bdev_flush`). `chown`/`chmod` pasan a funcionar en FAT32; reemplaza `fat32_ram_mode_*`. Renombrar y borrar mantienen la tabla. Tabla ausente o corrupta ⇒ valores por defecto actuales (root, 0755/0644) y aviso en el log; nunca fallo al montar.
- **U2.2** `fmask/dmask`/`uid=`/`gid=` como opciones de montaje para el caso en que no haya sidecar (USB de otro sistema): todo pertenece a un usuario concreto.
- **U2.3** **`/etc` persistente:** al arrancar, si existe `/data/etc/{passwd,shadow,group}`, se usan en lugar de la semilla de tarfs (el tmpfs de `/etc` se siembra desde ahí, ver `tmpfs.c:g_etc_seeds`); los cambios hechos por `passwd`/`adduser` se escriben ahí mediante la ruta de **escritura atómica** (`fichero.tmp` + `rename`) que ya usan las herramientas de shadow. Primera vez: se copian desde la imagen.
- **U2.4** Corregir la semilla de reserva para que coincida con `etc/passwd` (home de root, shell) y añadir `shadow`, `shells`, `login.defs` mínimo, `/etc/skel`.
- **U2.5** (Opcional, aparte) Esbozo del driver ext2 para `/data` si DU1-b gana; este plan solo deja el punto de montaje preparado.
- **Hito U2:** en QEMU: `chown 1000:1000 /data/x; chmod 600 /data/x`, apagar con `poweroff`, arrancar, y `stat` conserva uid/gid/modo. Un `passwd` de prueba editado a mano en `/data/etc` sobrevive al reinicio.
- **Riesgos:** corrupción de la tabla (por eso el renombrado atómico y los valores por defecto); `fsck` de Aurora (`aurora-fsck`) debe tolerar el fichero oculto; compatibilidad con Windows (el fichero se ve; marcar *hidden*).

### U3. Cuentas, contraseñas y herramientas (M)

- **U3.1** Construir contra glibc (`libs/glibc`): **shadow-utils** (`passwd`, `useradd/usermod/userdel`, `groupadd`, `chpasswd`, `su`, `login`), **util-linux** (`agetty`, `login`, `runuser`) y **libxcrypt** si hace falta `crypt()`. Añadir script `scripts/build_shadow.sh` (con fuentes bajo `third_party/`), todos con `--without-pam`. Si alguno resulta demasiado pesado, se sustituye por `aurora-login/su/passwd` propios (≈300 líneas cada uno).
- **U3.2** Instalar los binarios que necesitan setuid-root (`su`, `passwd`, `sudo`, `login`) con modo `4755` en el sysroot. **Verificar que tarfs conserva `04755`** (`tarfs.c:435` aplica `file_mode & 07777`, debería).
- **U3.3** Permisos de `/etc`: `shadow` `0640 root:shadow` (grupo `shadow` nuevo), `passwd`/`group` `0644`. `/etc/sudoers` (`%wheel ALL=(ALL) NOPASSWD: ALL` en el LiveUSB, DU2-a; con contraseña si no).
- **U3.4** Cuentas por defecto: `root` (bloqueada para login, sí con `su`), `alvaro` (1000, grupos `wheel users video input render audio`), `nobody`, y cuentas de sistema para servicios (`dbus`, `messagebus` si se porta). Hash real para `alvaro` generado en la construcción con `mkpasswd`/Python (nunca una contraseña en claro en el repo; contraseña inicial a definir por Alvaro, cambiable con `passwd`).
- **U3.5** Comprobar que glibc resuelve usuarios: `getpwnam`, `getpwuid`, `getgrnam`, `getgrouplist`, `initgroups`, `getlogin`, `ttyname`. Prueba `user/apps/nss_test`. `nsswitch.conf` solo con `files` (sin `systemd`/`dns` en `passwd`); glibc carga NSS dinámico (`libnss_files.so`): verificar que está en la ISO y que el cargador lo encuentra.
- **Hito U3:** en QEMU, desde un shell root: `useradd -m -d /data/home/test test`, `passwd test`, `su - test`, `id` (uid/gid/grupos correctos), `sudo id` desde `wheel` → uid 0, y `su` con contraseña errónea falla con retardo.
- **Riesgos:** `libcrypt`/`libxcrypt` en el toolchain glibc; dependencias de PAM al compilar (`--without-pam`); retardo de `login` tras fallo requiere `nanosleep` fiable (ya existe); `shadow` en tmpfs (resuelto en U2.3).

### U4. Login en consola e `init` por usuario (M)

- **U4.1** Revisar `/usr/bin/init` (cargado desde `main.c:1027`): mantiene PID 1 como root, lee `/etc/aurora.conf` y lanza `getty` en la consola (y por puerto serie si lo hay) en lugar de la terminal directamente. Si `getty` muere, `init` lo reinicia (con límite de reinicios rápidos).
- **U4.2** Cadena `init → getty → login → shell`: `login` verifica contra `shadow`, aplica `initgroups`+`setgid`+`setuid`, hace `chdir` a `$HOME`, pone `HOME/USER/LOGNAME/SHELL/PATH`, abre el TTY de control (`TIOCSCTTY`, `tty.c:482`) y deja `/dev/ttyN` a `0620 usuario:tty`.
- **U4.3** Autologin configurable (DU2): `/etc/aurora.conf` con `login=auto:alvaro` | `login=prompt` | `login=root`; cmdline `aur.login=...` manda sobre el fichero.
- **U4.4** `/etc/motd`, `/etc/issue`, `/var/run/utmp`/`wtmp` mínimos si algún programa los pide (`who`, `last`): primero comprobar quién los necesita, no hacerlos por adelantado.
- **U4.5** `poweroff`/`reboot` para usuarios no-root: pasan por `sudo` o por un helper con permiso (`wheel`), no por abrir el kernel a todos (hoy los hace root y `power.c` no comprueba nada: añadir comprobación de privilegio).
- **Hito U4:** QEMU arranca, muestra `login:`, `alvaro` entra con su contraseña, `id` da uid=1000, `ls /data/home/alvaro` funciona, no puede escribir en `/etc`, ni `kill` a un proceso de root. `aur.login=root` conserva el arranque actual (compatibilidad con `check_boot.sh`).
- **Riesgo alto:** el terminal gráfico actual corre como root y todos los scripts de prueba asumen root; hay que actualizarlos a `aur.login=root` o ejecutar con `sudo`. Mitigación: introducir `aur.login=root` como valor por defecto *durante* U4 y cambiar el defecto a `auto:alvaro` solo al terminar U5.

### U5. Dispositivos, grupos y sesión gráfica por usuario (L)

El hito que más se nota, y el más arriesgado para el trabajo de GPU ya hecho (LinuxKPI/NVIDIA).

- **U5.1** `devtmpfs`: permisos y propietarios por regla (`/etc/aurora-devperms` o tabla en el kernel): `/dev/dri/card*` `0660 root:video`, `renderD*` `0666`/`0660 root:render`, `/dev/input/*` `0660 root:input`, `/dev/fb0` `root:video`, `/dev/tty*` `root:tty`, `/dev/null/zero/random` `0666`, `/dev/kvm`/`nvidia*` `root:video`. Los nodos que crean los drivers `.kur` (`i915`, `nvidia`) pasan por la misma tabla.
- **U5.2** **Permisos DRM:** `DRM_MASTER` solo para el primer proceso que abre la tarjeta (el compositor); los demás con `render` no necesitan master. Comprobar que `drm_is_current_master`/`drm_master_*` de `kernel/drm` respetan `uid`. No exigir root para abrir `card0` si el usuario está en `video`.
- **U5.3** **Mini-seat** (estilo `seatd`): un servicio pequeño corre como root, abre `/dev/dri/card*` y `/dev/input/*`, y se los pasa por socket UNIX (`SCM_RIGHTS`, ya soportado) al compositor del usuario, que corre sin privilegios. Si es demasiado, alternativa: grupos `video`+`input` y abrir directo (más simple, menos fino). **Recomendación:** empezar por grupos, dejar el seat para después.
- **U5.4** `XDG_RUNTIME_DIR=/run/user/<uid>` (tmpfs, `0700`, creado por `login`/sesión), `XDG_SESSION_TYPE=wayland`, `WAYLAND_DISPLAY`. Sockets de Wayland y D-Bus de sesión dentro de ese directorio (hoy están en rutas globales: auditar `aurora-wayland.c`).
- **U5.5** `aurora-wayland` y la terminal se lanzan **desde la sesión del usuario** (script `aurora-session`), no desde `init` como root. El usuario del escritorio es el de `login`.
- **U5.6** Apps de escritorio con `$HOME` real: configuración, tema claro/oscuro y fondo guardados en `~/.config/aurora/`, no en rutas globales; valores por defecto de `/etc/skel`. (Enlaza con el modo claro/oscuro en vivo de GTK3/Qt.)
- **U5.7** Pantalla de inicio de sesión gráfica (greeter) con el estilo de AuroraOS: lista de usuarios + contraseña, enlazada con U4 (misma verificación). Opcional hasta que lo pida Alvaro.
- **Hito U5:** QEMU con `virtio-gpu`/`bochs`: login como `alvaro`, aparece el escritorio, `ps` muestra el compositor con uid 1000, `/dev/dri/card0` es `root:video 0660`, `alvaro` está en `video`, y un segundo usuario sin ese grupo **no** puede abrirlo. Repetir en la Portégé (i915) antes de dar el hito por bueno.
- **Riesgos:** (1) el compositor sin root puede perder acceso a `/dev/mem`, `ioperm`, MSI de usuario o `modeset` de NVIDIA, que hoy funcionan por ser root: inventariar con un `strace`-equivalente (`LOG` del kernel con `uid != 0 && EACCES`) antes de cambiar; (2) `drm.debug` y la autocarga de NVIDIA no deben cambiar; (3) **no tocar el camino GPU sin que Alvaro pruebe en hardware** (cautela con el PC real); `aur.login=root` debe seguir bastando para volver atrás.

### U6. Capabilities (M–L)

Sustituir «root o nada» por privilegios finos, con compatibilidad Linux.

- **U6.1** Modelo: `cap_permitted/effective/inheritable/bounding/ambient` (64 bits cada uno) en `process_t`; reglas de `execve` de Linux (transformación por UID: uid 0 mantiene todo; setuid root las gana; el resto las pierde). `capget/capset` reales (reemplazar el stub de `syscall.c:6117`), `prctl(PR_CAPBSET_READ/DROP, PR_CAP_AMBIENT, PR_SET_KEEPCAPS)`, `/proc/<pid>/status` con `CapInh/CapPrm/CapEff/CapBnd/CapAmb`.
- **U6.2** `capable(int cap)` en el kernel y migrar las ~20 comprobaciones `euid == 0` (inventario en `syscall.c`, `vfs.c`, `kmod_sys.c`, `process.c`) a la capacidad correcta: `CAP_CHOWN`, `CAP_DAC_OVERRIDE`, `CAP_DAC_READ_SEARCH`, `CAP_FOWNER`, `CAP_FSETID`, `CAP_KILL`, `CAP_SETUID/SETGID/SETPCAP`, `CAP_SYS_MODULE` (`init_module`), `CAP_SYS_ADMIN` (`mount`, `pivot_root`), `CAP_SYS_BOOT` (`reboot`), `CAP_SYS_TIME`, `CAP_SYS_RAWIO` (`ioperm`/`iopl`/`/dev/mem`), `CAP_SYS_NICE`, `CAP_NET_ADMIN`, `CAP_NET_RAW` (ping, raw sockets), `CAP_NET_BIND_SERVICE` (puertos <1024), `CAP_MKNOD`, `CAP_SYS_CHROOT`.
- **U6.3** Con `euid == 0` el proceso sigue obteniendo todas las capacidades por defecto: **comportamiento visible idéntico** al actual para root; los no-root no ganan nada nuevo salvo lo que se les conceda.
- **U6.4** Ficheros con capacidades (`setcap`, xattr `security.capability`): fuera de alcance; se usa setuid + `sudo` y, para `ping`, socket ICMP sin privilegios (`net.ipv4.ping_group_range`, que **no existe aún** en `sysctl.c`; habría que añadirlo).
- **U6.5** Relacionar las `AUR_CAP_*` de los drivers `.aur` (`aur.h:36`): cargar un `.aur`/`.kur` exige `CAP_SYS_MODULE`; el conjunto `AUR_CAP_*` de cada driver sigue siendo independiente (es sobre hardware, no sobre usuarios).
- **Hito U6:** `ping` y un servidor en el puerto 80 funcionan como `alvaro` solo con la capacidad (sin `sudo`) concedida por `capset`; sin ella, `EPERM`. Todo el resto de pruebas de root sigue pasando.
- **Riesgo:** cualquier comprobación olvidada deja una puerta abierta *o* rompe un programa que dependía de root: por eso U6 va después y con inventario de `euid == 0` hecho con `grep`.

### U7. Endurecimiento y pulido (M)

- **U7.1** `/proc`: `hidepid` opcional; `/proc/<pid>/mem`, `ptrace`: solo mismo uid o `CAP_SYS_PTRACE`. Comprobar quién usa `ptrace` hoy (debuggers, `strace`).
- **U7.2** Límites: `setrlimit/getrlimit` por proceso (si no están), `RLIMIT_NPROC`, `RLIMIT_NOFILE` por usuario (anti *fork bomb* básico).
- **U7.3** Cuotas de memoria por usuario: **no** en este plan; solo registrar la idea.
- **U7.4** Cambio rápido de usuario (DU5): varias sesiones, varios compositores, conmutación de VT/DRM master. Solo si Alvaro lo pide; depende de multi-seat de U5.3.
- **U7.5** Auditoría: registro en `klog` de logins fallidos/correctos, `su`/`sudo` y violaciones (`EPERM` en `init_module`, `mount`), consultable con `dmesg`.
- **Hito U7:** un usuario sin privilegios no puede ver el entorno de otro (`/proc/<pid>/environ`), ni adjuntarse con ptrace, ni agotar los PIDs del sistema.

### U8. Opcionales según lo que se porte

- **U8.1** Si `sudo` no entró en U3 (por PAM), portarlo aquí.
- **U8.2** PAM mínimo (`libpam` + `pam_unix`) solo si se porta un software que lo requiera (p. ej. un greeter externo, `polkit`).
- **U8.3** `polkit`/`logind`-shim para apps de escritorio que piden «autenticación»: ver `Plan_Wayland_Unificado.md`.
- **U8.4** Cifrado de `/data` por usuario: fuera de alcance.

## 5. Dependencias y orden

```
U0 ─► U1 ─► U2 ─► U3 ─► U4 ─► U5 ─► U6 ─► U7 ─► U8
              │                 ▲
              └─────────────────┘  (U5 necesita U2: home y ~/.config persistentes)
```

- **U1 antes que todo:** los permisos incorrectos no se ven mientras todo es root.
- **U2 antes que U3/U4:** sin persistencia, las contraseñas y homes se pierden.
- **U6 puede adelantarse a U5** si Alvaro prioriza `ping`/servidores sobre el escritorio; no depende de él.
- **Relación con otros planes:** `Plan_LinuxKPI.md` (L8/L9 siguen sin empezar; este plan no los toca, pero U5 depende de que `/dev/dri` y los nodos de `nvidia`/`i915` sigan funcionando), `Plan_NVIDIA.md` (nodos `nvidiactl`/`nvidia0` con propietario `video`), `Plan_Wayland_Unificado.md` (sesión, seat, D-Bus), `Plan_Power.md` (permisos de `reboot`/`poweroff`).

## 6. Estrategia de pruebas

| Capa | Cómo |
|---|---|
| Unitaria (kernel) | `cred_test`, `perm_test`, `nss_test`, `caps_test` en `user/apps/`, ejecutados por `tests_thread` y `scripts/check_boot.sh` |
| Integración | script `scripts/users-test.sh` en QEMU: arranca con `aur.login=prompt`, introduce usuario/contraseña por el puerto serie del monitor/`-serial`, ejecuta una batería y verifica la salida |
| Regresión | las pruebas existentes se ejecutan con `aur.login=root`; deben seguir en verde en cada hito |
| Hardware | Portégé (i915) para U5; RTX 4060 solo tras confirmar con Alvaro (su PSU manda) |

## 7. Riesgos globales

1. **Romper el escritorio y la GPU** al bajar privilegios (U5): mitigado con `aur.login=root`, hitos pequeños y pruebas en QEMU antes de hardware.
2. **Pérdida de datos en `/data`** por el *sidecar* (U2): renombrado atómico, copia de seguridad `.aurora-meta.bak`, valores por defecto si falla.
3. **Contraseñas inseguras en la imagen:** nunca contraseñas en claro en el repo; hash generado al construir; contraseña inicial elegida por Alvaro.
4. **LiveUSB sin persistencia** si no hay partición `/data` escribible: el sistema debe seguir arrancando con las cuentas de la imagen (solo lectura) y avisar.
5. **Divergencia glibc/Aurora:** NSS dinámico, `crypt` y `initgroups` de glibc se prueban en cada hito de U3 (`libs/glibc`).
6. **Alcance:** U5–U6 son grandes; se pueden parar en U4 con un sistema ya usable (login y permisos reales) sin comprometer lo demás.

## 8. Resumen de hitos (para seguimiento)

| Hito | Contenido | Tamaño | Prueba |
|---|---|---|---|
| U0 | Pruebas de línea base | S | `cred_test`, `perm_test` pasan |
| U1 | `fsuid`, sticky, kill, `AT_SECURE`, `no_new_privs`, `chown` fino | M | tests ampliados |
| U2 | Metadatos persistentes + `/etc` persistente | M–L | `chown` sobrevive al reinicio |
| U3 | shadow-utils/util-linux (glibc) o `aurora-login/su/passwd` + NSS | M | `su -`, `sudo id`, `id` |
| U4 | `init → getty → login → shell`, autologin | M | login en QEMU |
| U5 | Propietarios de `/dev`, sesión gráfica por usuario | L | escritorio como uid 1000 |
| U6 | Capabilities reales | M–L | `ping` sin root |
| U7 | `hidepid`, `ptrace`, límites, auditoría | M | pruebas de aislamiento |
| U8 | `sudo`/PAM/polkit si hacen falta | — | según el software |

**Primer paso propuesto:** U0 + U1 (sin tocar el arranque ni la GPU), y decidir DU1 antes de U2.
