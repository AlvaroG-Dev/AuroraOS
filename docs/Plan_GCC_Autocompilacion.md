# Plan_GCC_Autocompilacion — GCC en AuroraOS y compilar AuroraOS desde AuroraOS

Fecha: 2026-11-01. Solo plan (sin código). Rama `claude/plan-gcc`, sin fusionar.
Las cosas marcadas **[verificado]** se comprobaron leyendo el repo en `main` (086b118b); las marcadas **[por medir]** son estimaciones que hay que confirmar en QEMU antes de fiarse.

---

## 0. Objetivo, alcance y criterio de éxito

**Objetivo final (self-hosting):** arrancar `aurora.iso` en una máquina (o QEMU) y, sin ayuda del host, ejecutar `make iso` dentro de AuroraOS y obtener una `aurora.iso` que arranca.

Dos niveles, porque no cuestan lo mismo:

| Nivel | Qué significa | Cuándo se da por logrado |
|---|---|---|
| **Compilador nativo** | `gcc hello.c && ./a.out` funciona dentro de AuroraOS; también `g++`, `make`, `ld`, `as`, `ar`. | Hito H2 |
| **Autocompilación del toolchain** | Dentro de AuroraOS se compila binutils y GCC desde fuentes, y el GCC resultante vuelve a compilar GCC (bootstrap de 3 etapas, etapas 2 y 3 idénticas). | Hito H5/H6 |
| **Autocompilación del SO** | Dentro de AuroraOS: kernel + userland propio + initrd + imagen EFI + ISO híbrida. | Hito H7 (kernel) y H8 (ISO) |

**Fuera de alcance de este plan:** compilar dentro de AuroraOS los componentes de terceros pesados (Mesa/LLVM, Qt, GTK, Weston, Xwayland, Python, Perl, el driver NVIDIA…). Hoy llegan a la ISO como binarios del host (`scripts/fetch-*.sh`, `precompiled/`, `libs/glibc/`); en el self-hosting se **conservan como artefactos precompilados** y solo se reconstruye lo que es de AuroraOS. Lo explico en §7.

---

## 1. Qué hay hoy (verificado)

### 1.1 Cómo se construye hoy todo

- **Host:** Ubuntu 24.04 con `gcc 13.3.0` (`/usr/bin/gcc`, `x86_64-linux-gnu-gcc`) y glibc 2.39.
- **`make iso`** (`Makefile`, 1231 líneas) encadena: `env` (`scripts/setup-env.sh`: apt de `build-essential binutils nasm clang lld git curl file xxd qemu-system-x86 ovmf mtools dosfstools xorriso gdisk python3 python3-pil librsvg2-bin …`) → `sysroot` (copia `assets/` y `etc/`) → `user` (nativo + `user/musl`) → `drivers` (`.kur`/`.aur` con `tools/kurbuild`/`aurbuild`, LinuxKPI, NVIDIA) → `initrd.tar` → `kernel` → `build/efi.img` (FAT16 con `mkfs.fat` + `mmd`/`mcopy`) → `xorriso -as mkisofs` + varios `dd` para el MBR híbrido y la partición `AURORADATA` de `/data` (FAT32, `USB_DATA_MB=64`).
- **Compiladores que usa el build:**

| Pieza | Compilador | Observaciones |
|---|---|---|
| `kernel/` | `x86_64-elf-gcc` si existe, si no `gcc` del host, con `-ffreestanding -mcmodel=kernel -mno-red-zone -mno-avx*…` (`kernel/Makefile`) y `nasm` para los `.asm` | GCC normal basta; **no** hace falta un cross `x86_64-elf` (el Makefile ya tiene la rama `CROSS=` vacía). |
| `user/` (nativo) | igual que el kernel (`-nostdlib`, `-T link.ld`) | |
| `user/musl/` y `tcc` | `toolchain/x86_64-linux-musl-cross` (287 MB, GCC cross de musl; **solo** para binarios musl existentes, p. ej. `tcc`) | La libc "principal" del sistema es glibc; esto es un resto histórico. |
| `drivers/` `.kur/.aur` | `gcc` host vía `tools/kurbuild`, `tools/aurbuild` (C) | |
| `bootloader/efi` | **`clang` + `lld-link`** (`-target x86_64-windows`/PE) con `gnu-efi` | **Bloqueo para el self-hosting:** GCC no genera PE/COFF para Linux host sin un cross-mingw. Ver §6.4. |
| `bootloader/bios` | `nasm` + `gcc -m32` | Necesita `gcc -m32 -ffreestanding` (sin multilib de libc: solo freestanding). |
| Apps Wayland/GTK/Qt, `aurora-wayland` | `scripts/build-aurora-wayland.sh` etc. contra `.deb` de Ubuntu descargados | No self-hosteables a corto plazo (§7). |

- **Userland binario:** `scripts/fetch-bin.sh` copia binarios del host (bash, coreutils, grep, sed, gawk, tar, gzip, bzip2, xz, find, diff, git, perl, python3.12, openssl, nano, tmux…) y resuelve sus `.so` con `ldd` a `libs/glibc/` (156 MB; `ld-linux-x86-64.so.2` en `lib64/`). **La glibc que hay en la ISO es la 2.39 de Ubuntu, sin archivos de desarrollo** (solo runtime). `make`, `ld`, `as`, `ar`, `gcc`, `m4`, `bison`, `flex`, `patch`, `xorriso`, `mtools`, `nasm`, `cc1` **no** están en `precompiled/usr/bin` hoy.
- **initrd:** ~500 MB, **cargado entero en RAM** por el firmware (comentario del Makefile: con 512 MB de RAM no arranca; `QEMU_MEM=2G` por defecto). Todo lo que se meta en el initrd ocupa RAM para siempre. `sysroot/usr/include` ya lo usa `tcc` con cabeceras de **musl**, lo que choca con las de glibc (ver G0.2).

### 1.2 Kernel: lo relevante para un compilador

| Área | Estado | Efecto para GCC |
|---|---|---|
| ABI | Linux x86-64; ejecuta bash, coreutils, Perl 5.38, Python 3.12, Git 2.43 con glibc (`glibc_port.md` §2, con la salvedad de que es documentación del propio proyecto) | La ABI ya es la de `x86_64-linux-gnu`. |
| Syscalls | ~210 números definidos en `kernel/syscall.h`; presentes (por `grep`): `rename`, `symlink`, `readlink`, `link`, `utimensat`, `fsync`, `flock`, `fcntl`, `pipe2`, `vfork`, `wait4`, `prlimit64`, `ftruncate`, `fallocate`, `mremap`, `mprotect`, `madvise`, `clock_gettime`, `getdents64`, `fchdir`, `mknod`, `getrusage`, `times`, `sysinfo`, `pread64/pwrite64`, `sendfile`, `statfs/fstatfs`, `readv/writev`, `dup3`, `ppoll`, `pselect6`, `set_robust_list`, `rseq`, `arch_prctl`, `getrandom`, `chmod/fchmod`, `chown/lchown`, `prctl` | Cobertura amplia. |
| Syscalls que **no encontré** con un `grep` basto (comprobar antes de dar por ausentes) | `renameat2`, `fdatasync`, `copy_file_range`; `mkfifo` pasa por `mknod` | `cp`/`mv`/`install` de coreutils caen a alternativas con `ENOSYS`; `fdatasync` puede mapearse a `fsync`. Bajo riesgo. |
| `fork` | `sys_fork` **copia todas las páginas, sin COW** (`kernel/process.c:1928`, "lento pero simple"). Existe `vfork` y `clone`/`clone3` con fallback. | `configure` y `make` hacen decenas de miles de `fork+exec`; funciona, pero lento (G1.2 mide, G8 propone COW). |
| Límites por proceso | `MAX_PROCESS_FDS=256`; pila 8 MiB; `rlimits[16]` por proceso | Suficiente salvo `make -jN` con jobserver y `ld` con muchos objetos; subir a 1024 es trivial. |
| **`VFS_PATH_MAX = 128`** | Ruta completa (cwd, `node->name`, `exe_path`) y nombre de dirent, en `kernel/vfs.h:60` | **Bloqueo duro.** Rutas como `/data/build/gcc-13.3.0/x86_64-linux-gnu/libstdc++-v3/include/bits/…` superan 128 bytes; GCC, glibc y el árbol de fuentes de `kernel/` también. Hace falta `PATH_MAX=4096`. `Plan_Ext2.md` D5 lo deja fuera; aquí pasa a ser **requisito**. |
| `rename` | `vfs_rename` **no sobrescribe** (`-EEXIST`) según `Plan_Ext2.md` §1 | **Bloqueo duro.** `ld`, `ar`, `as`, `make`, `mv`, `install`, `config.status` y `libtool` dependen de reemplazar ficheros. |
| `uname` | `sysname = "Aurora"` (`kernel/syscall.c:2257`) | Se mantiene: con el triplete propio `config.guess` lo reconocerá (§2.2). Hasta entonces, pasar `--build=`/`--host=`. |
| `/tmp` | tmpfs de **64 MB** (`kernel/tmpfs.c:1001`), `/var` 16 MB, `/run` 4 MB, `/etc` 4 MB; todo en RAM | `cc1` para ficheros grandes, `-flto`, `ar`/`ld` intermedios y `tar` ya no caben; usar `TMPDIR` en disco. |
| Memoria virtual | `mmap`/`mremap`/`mprotect`/`madvise` presentes; `mmap_race`, `vma_*` tests | `cc1plus`/`lto1` piden cientos de MB de VA y RSS de 0,5–2 GB para compilar GCC (§4.3). **[por medir]** |
| Almacenamiento | FAT32 RW con LFN, sin symlinks, sin hard links reales, sin propietarios/modos (modos solo en RAM), mtime en segundos, `/data` por defecto de 64 MB (`USB_DATA_MB`), buffer cache de 256×512 B (`fat32.c`), sync diferido | Insuficiente para un build real, sobre todo por los symlinks y el espacio (§4.4). |
| Almacenamiento (futuro) | `Plan_Ext2.md` (rama `claude/plan-ext2`, E0–E8) propone ext2 completo con symlinks, hard links, uid/gid/modos, rename con sobrescritura, `mount(2)`, caché de bloques y 4 KiB de bloque | Es la pieza que desbloquea el self-hosting real (§4.4). |
| `/proc` | `stat`, `status`, `maps`, `smaps`, `mountinfo`, `meminfo`, `vmstat` | Suficiente para `ps`, `top`, `make -j$(nproc)` (si `nproc` lee `sched_getaffinity` o `/proc/cpuinfo`: **por comprobar**). |
| Pipes, futex, señales, PTY | Implementados y probados con bash/Python | `make -j` usa pipes (jobserver), `gcc` usa pipes y `wait4`; sin novedad. |

---

## 2. Decisión de fondo: triplete propio `x86_64-pc-aurora-gnu`

**Decisión de Alvaro (2026-11-01): AuroraOS tiene su propio triplete y el toolchain se identifica como AuroraOS, no como Linux.** Triplete: **`x86_64-pc-aurora-gnu`** (kernel `aurora`, libc `gnu` = glibc; mismo esquema que `x86_64-pc-kfreebsd-gnu`, que `config.sub` ya sabe leer). Alternativa más corta: `x86_64-aurora` (os=`aurora`, libc implícita).

### 2.1 Qué cambia y qué NO cambia

- **No cambia la ABI en ejecución.** Los binarios siguen siendo ELF x86-64 que llaman a la glibc 2.39 precompilada y al kernel por syscalls Linux. El triplete solo afecta a las **herramientas** (rutas, macros predefinidas, nombres). Por eso glibc, Perl, Python, Wayland… precompilados siguen funcionando sin recompilar.
- **Sí cambia la identidad visible:** `gcc -dumpmachine` → `x86_64-pc-aurora-gnu`; `gcc -dM -E` define `__aurora__` (y `__AURORAOS__`); rutas `/opt/aurora-tc/lib/gcc/x86_64-pc-aurora-gnu/13.3.0`; `uname -s` = `Aurora` (se mantiene); `/etc/os-release` con `ID=aurora`.
- **Compatibilidad de macros (D3, decidido: se definen ambas):** muchos programas usan `#ifdef __linux__` para elegir código que AuroraOS sí soporta (epoll, `/proc`, `getrandom`…). Recomiendo que el GCC de AuroraOS defina **`__aurora__` y además `__linux__`/`__gnu_linux__`/`__unix__`** (igual que otros derivados que reutilizan la ABI), de modo que el protagonismo de la marca sea de Aurora en nombre, triplete y herramientas, pero el software ajeno compile sin parchear. La alternativa (no definir `__linux__`) obliga a parchear cada paquete de terceros.

### 2.2 Parches pequeños necesarios (lista cerrada)

Todos son adiciones mínimas copiando lo que ya existe para `linux-gnu`; ninguno toca el generador de código. Se guardan como `patches/` en el repo, versionados por paquete:

| Paquete | Ficheros | Cambio |
|---|---|---|
| **config.sub / config.guess** (copia común, usada por todos) | `config.sub`, `config.guess` | Aceptar `aurora*` como kernel/OS; `config.guess` devuelve `x86_64-pc-aurora-gnu` si `uname -s` = `Aurora`. Una única copia mantenida en `third_party/aurora-config/` y un script `scripts/aurora-autoreconf.sh` que la copia sobre cualquier árbol de fuentes antes de `configure`. |
| **binutils** | `bfd/config.bfd`, `gas/configure.tgt`, `ld/configure.tgt`, `ld/emulparams/` | Nueva fila `x86_64-*-aurora*` que reutiliza `elf_x86_64` (y `elf_i386`) y los mismos `targ_defvec`. Ruta del cargador por defecto `/lib64/ld-linux-x86-64.so.2` sin cambios. |
| **GCC** | `gcc/config.gcc`, nuevo `gcc/config/aurora.h`, `gcc/config/i386/aurora64.h`, `libgcc/config.host`, `fixincludes/mkfixinc.sh`, `libstdc++-v3/configure.host`, `libgomp`/`libatomic` hosts | `aurora.h` incluye `gnu-user.h`/`linux.h` (y por tanto `crt1.o`, `ld.so`, `libc.so` como hoy) y añade `-D__aurora__ -D__AURORAOS__` y la identidad `TARGET_OS_CPP_BUILTINS`. `libstdc++` usa `config/os/gnu-linux`. |
| **glibc** | **ninguno** mientras use la precompilada de Ubuntu | Si algún día se recompila glibc, habrá que parchear `sysdeps/` (su configure detecta `linux`); se pospone (fuera de alcance). |
| **autoconf generados de terceros** | `configure` de cada paquete | Se resuelve con la copia común de `config.sub`; para los pocos que prueban `*-linux*` en sus `configure.ac` (p. ej. libtool, `ltmain.sh`) se añade `*-aurora*` por `sed`. Lista a mantener en `docs/Paquetes_Aurora.md`. |
| **libtool** | `ltmain.sh`/`libtool.m4` | Tratar `aurora*` como `linux*` (soname versioning, `-rpath`). Es la fuente habitual de enlazado estático "silencioso"; hay que probar `.so.N.M` en G3. |
| **make, bash, coreutils, tar, sed…** | solo `config.sub/guess` | Nada más. |
| **kernel** | `uname` ya es "Aurora" | `sysctl kernel.ostype=Aurora`, `/proc/version` y `/etc/os-release` coherentes. |

### 2.3 Coste y riesgos de esta decisión (en claro)

- Cada paquete nuevo que se quiera compilar dentro de AuroraOS necesita el paso de **config.sub/guess** (automatizado) y, ocasionalmente, un `sed` a su `configure`. Con `--build=x86_64-pc-aurora-gnu` explícito se evita `config.guess`, pero `config.sub` debe conocer `aurora`.
- Paquetes con `case $host in *-linux*)` en `configure.ac`/`meson.build`/`CMake` (p. ej. algunos con detección de `epoll`, `-ldl`, versiones de símbolos, `-Wl,--version-script`) pueden caer a una rama genérica y compilar sin una característica. Mitigación: tabla de excepciones y una prueba por paquete; si hay muchos, el atajo es pasar `--host=x86_64-pc-linux-gnu` solo para esos paquetes (se documenta como excepción, no como norma).
- Los binarios Ubuntu del Escalón A (G2) siguen identificándose como `x86_64-linux-gnu` (rutas `lib/gcc/x86_64-linux-gnu/13`). Está bien: es la fase de pruebas del kernel. El renombrado a `aurora` llega con el toolchain propio (G5). Mientras tanto se enlaza `lib/gcc/x86_64-pc-aurora-gnu → x86_64-linux-gnu` si hace falta.
- Mantener parches en GCC 13.x implica rehacerlos al subir de versión (pocos ficheros; el diff estimado es de < 300 líneas entre GCC, binutils y config.sub). Fijar GCC 13.3 y binutils 2.42 hasta nuevo aviso.
- **No** musl y **no** busybox (decisión previa del proyecto). El `toolchain/x86_64-linux-musl-cross` de 287 MB queda solo para lo musl que exista (tcc); puede retirarse cuando tcc deje de ser necesario (G9).

## 3. Qué hay que llevar a AuroraOS (inventario)

### 3.1 Toolchain mínimo para compilar C (hito H2)

| Paquete (versión Ubuntu 24.04) | Qué trae | Notas |
|---|---|---|
| `binutils` 2.42 | `as`, `ld` (bfd; `ld.gold` opcional), `ar`, `ranlib`, `nm`, `objdump`, `objcopy`, `strip`, `readelf`, `size`, `strings`, `addr2line` | Autónomo, solo depende de glibc y zlib/zstd. |
| `gcc-13` / `cpp-13` | `gcc` (driver), `cc1`, `collect2`, `lto-wrapper`, `lto1`, `liblto_plugin.so`, libgcc, `crtbegin*.o`/`crtend*.o`, cabeceras internas (`stddef.h`, `stdarg.h`, `x86intrin.h`…), `libgcc_s.so`, `libgomp`, `libatomic`, `libasan` (opcional) | `cc1` depende de `libgmp`, `libmpfr`, `libmpc`, `libisl`, `libzstd`, `libz`. |
| `g++-13` / `libstdc++-13-dev` | `cc1plus`, `libstdc++.so/.a`, cabeceras C++ | Necesario para compilar GCC ≥ 4.8 (que es C++) y para la mayor parte de lo demás. Opcional en H2, imprescindible en H5. |
| `libc6-dev` 2.39 | `/usr/include` (glibc), `crt1.o`, `crti.o`, `crtn.o`, `Scrt1.o`, `gcrt1.o`, `libc.so` (script de enlace), `libc_nonshared.a`, `libm.a`, `libc.a`, `libpthread.a`, `libdl.a`, … | **Hoy no está en `libs/glibc/`** (solo runtime). |
| `linux-libc-dev` | cabeceras del kernel Linux (`linux/*.h`, `asm/*.h`, `asm-generic/`) | AuroraOS no tiene cabeceras propias para esto; las de Linux son las que describen su ABI. |
| `libgmp`, `libmpfr`, `libmpc`, `libisl` (runtime + dev) | Aritmética de GCC | Solo si se compila GCC desde fuentes (H5): GMP 6.3, MPFR 4.2, MPC 1.3, ISL 0.26 (o `--with-system-*`). |
| `make` 4.3 | | Debe funcionar con `--jobserver-auth` (pipes) y `sh` = bash o dash. |
| `bash`, `dash`, `coreutils`, `findutils`, `diffutils`, `grep`, `sed`, `gawk`, `tar`, `gzip`, `bzip2`, `xz`, `patch` | Sustrato de `configure` y `make` | La mayoría **ya está**; faltan `patch`, `m4`, y revisar `install`, `ln`, `expr`, `test`, `uniq`, `cmp`, `sort`, `tr`, `mktemp`, `basename`, `dirname` (la lista de `precompiled/usr/bin` ya los tiene). |
| `m4`, `bison`, `flex`, `texinfo`/`makeinfo` | Solo si se regeneran fuentes (`make dist` de GCC no lo exige) | GCC 13.3 en tarball se compila sin bison/flex/texinfo si no se tocan `.y/.l/.texi` ni se hace `make maintainer-clean`; `--disable-doc` evita `makeinfo`. |
| `perl` 5.38, `python3` | `perl` está; `python3` ya está | Para glibc (`python3`), `autoconf`, tests de GCC (DejaGnu: `expect`, `tcl`, solo si se corre `make check`). |
| `pkg-config`, `autoconf/automake/libtool` | Solo para proyectos que no traen `configure` generado | Para el kernel/AuroraOS no hacen falta. |

### 3.2 Para compilar AuroraOS dentro de AuroraOS (hitos H7–H8)

| Herramienta | Para qué | Estado |
|---|---|---|
| `make` + `gcc` + `binutils` | kernel, user, drivers `.kur/.aur` | Ver 3.1 |
| `nasm` | `kernel/*.asm`, `user/crt0.asm`, bootloader BIOS | Pequeño, C puro; **portar desde fuentes** (G6) o llevar el binario de Ubuntu. |
| `python3` (+ `python3-pil` y `librsvg2-bin` solo para iconos) | `scripts/*.py`, generación de iconos y fondos | `python3` ya está. **Los PNG/BMP/iconos generados se commitean o se llevan precompilados**, no se regeneran dentro del SO (H8 usa `assets/` ya generados). |
| `mtools` (`mcopy`, `mmd`), `dosfstools` (`mkfs.fat`), `gdisk`/`sfdisk`, `xorriso` | Construir `efi.img`, la ISO híbrida y la partición `/data` | Ninguno está hoy. Todos son C, dependen de glibc/zlib; portar o llevar binarios Ubuntu. `xorriso` necesita `libburn/libisofs/libisoburn` (están en el `.deb`). |
| `clang` + `lld-link` | **Solo** `bootloader/efi` | Ver 6.4: no se lleva LLVM. |
| `git` | traer el código | Ya está (Git 2.43). |

### 3.3 Qué NO hay que llevar

LLVM/Mesa, Qt, GTK, Weston, Xwayland, Wayland, ICD de Vulkan, NVIDIA open-gpu-kernel-modules, linux-firmware, y el árbol de Linux fijado que usa LinuxKPI (varios cientos de MB). El self-hosting recompila el **núcleo del sistema** (kernel base + userland propio + drivers `.kur/.aur` pequeños + bootloader + empaquetado); el resto se toma de una **caché de artefactos** (§7).

---

## 4. Requisitos al kernel y al sistema

### 4.1 Obligatorios antes de compilar nada serio (bloqueos)

| # | Requisito | Por qué | Esfuerzo |
|---|---|---|---|
| K1 | `VFS_PATH_MAX` → 4096 (cwd, `exe_path`, nombres de nodo, `vfs_dirent_t.name` ≥ 256); revisar `kmalloc` por nodo y `tmpfs` | rutas del árbol de build | M (toca `vfs_node_t`, tmpfs, tarfs, fat32, procfs; ver `Plan_Ext2` D5) |
| K2 | `rename` con sobrescritura atómica (fichero sobre fichero; directorio vacío sobre directorio vacío) y `renameat2` (`RENAME_NOREPLACE`) | patrón estándar de toolchain | S–M (comparte E0 de `Plan_Ext2`) |
| K3 | symlinks y hard links persistentes en el disco de trabajo | `libstdc++.so.6 → …6.0.32`, `cc → gcc`, `libtool`, `make install`, glibc | los da ext2 (E3) |
| K4 | `/tmp` grande o `TMPDIR` en disco; subir `tmpfs` de `/tmp` a ≥ 25 % de la RAM (dinámico) o 512 MB | `cc1`, `ld`, `ar`, `tar` | S |
| K5 | `MAX_PROCESS_FDS` → 1024 y `RLIMIT_NOFILE` coherente | `make -j`, `ld` | S |
| K6 | `config.sub`/`config.guess` con `aurora` (§2.2) y `ostype`/`os-release` coherentes | autoconf | S |

### 4.2 Muy recomendables (rendimiento y robustez)

- **COW en `fork`** (hoy copia todo): `configure` de GCC hace ~15 000 `fork+exec`; bash con ~5–10 MB de RSS se copia cada vez. Con `vfork`/`posix_spawn` de glibc (que usa `clone(CLONE_VM|CLONE_VFORK)`) muchas ya evitan la copia: **[por medir]** cuántas llamadas son `fork` real. Si el `configure` de binutils tarda más de ~5× lo del host en el mismo QEMU, COW pasa a ser G8.
- **Caché de bloques/páginas genérica** (la de ext2, `Plan_Ext2` §3.3) y **`mmap` de ficheros** con `MAP_PRIVATE` coherente: `ld` mapea los objetos de entrada y `cc1`/`as` leen con `mmap` en algunas rutas; `get_page` está en `vfs_ops_t`, **pero hay que comprobar que `mmap` de fichero sobre ext2/FAT32 funciona con `MAP_PRIVATE` y `MAP_SHARED` de lectura** (G1.1).
- **Tiempos de fichero con resolución mejor que 1 s** (`utimensat` ya existe): `make` compara `mtime`; con mtime de FAT32 en segundos y 2 s de resolución en el dirent original, un build rápido produce **reconstrucciones espurias o, peor, cosas "ya actualizadas" cuando no lo están**. ext2 con inodos de 256 B guarda nanosegundos (extra fields); con 128 B solo segundos, y `make` 4.3 con segundos funciona pero con el *timestamp-skew* clásico. Recomendación: inodos de 256 B (`mke2fs -I 256`). `ctime` y `atime` también (hoy `vfs_stat_t` solo trae `mtime`, `Plan_Ext2` §3.1).
- **`sched_getaffinity`/`nproc`/`/proc/cpuinfo`** coherentes con SMP para `make -j`.
- **Reloj monótono estable:** ver `Evaluacion_Reloj_Kernel.md` en los ficheros del proyecto; `make` y `ld` no sufren si hay saltos, pero `ccache`/`ninja` sí. No es bloqueo.
- **`ptrace` mínimo** solo si se quiere `gdb`/`strace`; no es necesario para compilar.

### 4.3 Memoria

| Tarea | RAM aprox. **[por medir]** |
|---|---|
| `gcc hello.c` | < 100 MB |
| compilar `make`/binutils (`-O2`) | 200–500 MB |
| compilar `cc1plus` y `gcc/` de GCC (`-O2`, `-j1`) | ~1 GB en picos (`insn-attrtab.c`, `gimple-match-*.c`: 700 MB–1,5 GB por TU) |
| `lto1` / bootstrap con LTO | > 4 GB (no usar LTO en el bootstrap; `--disable-lto`) |
| enlazar `cc1plus` | 500 MB–1 GB |
| `make -j4` de GCC | 4–6 GB |

El initrd de ~500 MB ocupa RAM fija, `/tmp` es tmpfs, y el kernel no tiene swap en producción (existe `swap.c`, **por comprobar** que sea utilizable). Por tanto: máquina de pruebas del self-hosting con **8 GB de RAM** (QEMU `-m 8G`) como mínimo razonable para H5/H6; para H2/H3 bastan 2–3 GB. Poner `OOM` en la lista de riesgos: si el kernel mata o se cuelga con 95 % de RAM, el bootstrap fallará de forma opaca (G1.3 prueba ese caso).

### 4.4 Almacenamiento

**Estimación de espacio [por medir]:**

| Contenido | Tamaño |
|---|---|
| Toolchain instalado (gcc+g++ +binutils+libc6-dev+libstdc++-dev, sin docs/locales) | 250–400 MB |
| Fuentes: GCC 13.3 (+ GMP/MPFR/MPC/ISL) 150 MB tar → 700 MB descomprimidas; binutils 2.42 ~ 120 MB; make 4 MB; glibc (si algún día) 1 GB | ~1 GB |
| Árbol de build de GCC (`--disable-bootstrap`: ~3–4 GB; bootstrap completo: 8–10 GB; con `--disable-multilib --enable-languages=c,c++`) | 4–10 GB |
| Fuentes de AuroraOS (sin `third_party`, sin LinuxKPI ni NVIDIA) + objetos | 0,5–1 GB |
| Imagen de la ISO generada | 0,6–1 GB |
| **Total cómodo** | **≥ 16 GB**, mejor 32 GB |

Consecuencias:

1. El `/data` por defecto de 64 MB (parte de la ISO/USB) no sirve. El self-hosting trabaja con un **disco de trabajo aparte** (disco virtual QEMU de 32 GB, o partición en el disco real), montado en `/work` (o `/data`).
2. FAT32 sirve para H1 (hello world, sin symlinks) pero **no para H3+**: sin symlinks reales, sin hard links, sin modos (el bit de ejecución se simula en RAM), mtime de 2 s. La ruta que recomiendo es **ext2 (Plan_Ext2 E0–E3) como sistema de ficheros de `/work`**. No es necesario esperar a E4–E8 (consistencia, instalador).
3. **Orden de dependencias entre planes:** `Plan_Ext2` E0 + E1 + E2 + E3 → este plan G3 en adelante. G0–G2 se pueden hacer antes, sobre FAT32 y tmpfs (con las limitaciones anotadas), y de hecho **sirven para depurar el kernel** (cada fallo de `configure` es un test de compatibilidad).
4. `Plan_Usuarios` (uid/gid/permisos): compilar como `root` en `/work` es suficiente al principio; `make install` con `--prefix=/usr/local` no requiere usuarios. A medio plazo conviene compilar como usuario normal (U1–U4) para no estropear el sistema con un `rm -rf` mal puesto.
5. **Dónde viven las herramientas:** ni en el initrd (RAM fija, +300 MB) ni en `/data` de 64 MB. Opciones: (a) un segundo disco "toolchain" (imagen ext2 `aurora-devtools.img`, montada en `/opt/devtools`) generada por el Makefile del host; (b) una partición adicional del mismo disco; (c) un `.tar.xz` que se extrae a `/work` la primera vez. Recomiendo (a) para QEMU y (b) para hardware real. D5.

---

## 5. Estrategia de bootstrap

Tres escalones; cada uno es un hito con prueba objetiva.

**Escalón A — usar las herramientas de Ubuntu tal cual (G0–G3).** Como AuroraOS ya ejecuta binarios Linux/glibc, se copian al disco de herramientas los `.deb` ya compilados (`gcc-13`, `binutils`, `make`, `libc6-dev`, `libstdc++-13-dev`, `linux-libc-dev`, gmp/mpfr/mpc/isl) con una variante de `scripts/fetch-bin.sh` (`scripts/fetch-devtools.sh`, que use `apt-get download` + `dpkg -x` sin root, como ya hace `fetch-wayland.sh`). Esto prueba el kernel con el compilador más exigente imaginable **sin escribir un solo parche de toolchain**. Es también el plan B que siempre funciona.

**Escalón B — construir en el host un toolchain propio que corra en AuroraOS (G4–G5).** Con el triplete propio (§2) esto **sí es un cross-compilador real**: en el host se compila binutils+GCC parcheados con `--build=x86_64-pc-linux-gnu --host=x86_64-pc-aurora-gnu --target=x86_64-pc-aurora-gnu --with-sysroot=<sysroot AuroraOS>`. Como la ABI de ejecución es la de glibc/Linux, el resultado corre en AuroraOS y es el primer toolchain "Aurora" de verdad. Sirve para: (1) fijar versiones exactas y flags (`--disable-multilib`, `--with-sysroot=/`, `--enable-languages=c,c++`, `--disable-nls`, `--disable-libsanitizer`, `--enable-default-pie`, `--with-system-zlib`), (2) generar un tarball reproducible `aurora-toolchain-13.3.0.tar.xz` independiente de la versión de Ubuntu, (3) tener un binario de referencia contra el que comparar (§8).

**Escalón C — que AuroraOS se compile a sí misma (G5–G8).**

```
host:   gcc 13.3 (Ubuntu)  ──►  [1] toolchain-A (Ubuntu .deb, copiado)
AuroraOS:                          toolchain-A compila ──► [2] binutils/make/… (stage "native-1")
AuroraOS:                          toolchain-A compila ──► [3] GCC 13.3 (+ binutils) "stage1"
AuroraOS:                          stage1 compila   ──► [4] GCC 13.3 "stage2"
AuroraOS:                          stage2 compila   ──► [5] GCC 13.3 "stage3"
                                   comparar stage2 == stage3 (bootstrap-compare de GCC)
AuroraOS:                          gcc nativo compila ──► kernel + userland + drivers + bootloader BIOS
AuroraOS:                          xorriso/mtools/mkfs.fat ──► aurora.iso
```

- **Con triplete propio hace falta un cross (Escalón B) y, para dejar de depender del host, un canadian cross:** `--build=x86_64-pc-linux-gnu --host=x86_64-pc-aurora-gnu --target=x86_64-pc-aurora-gnu` produce el GCC nativo de AuroraOS (stage0); ese stage0, ya dentro de AuroraOS, compila stage1/2/3 con `--build=--host=--target=x86_64-pc-aurora-gnu`. Es más trabajo que antes, pero es lo que da identidad propia.
- Bootstrap de 3 etapas dentro de AuroraOS (`make bootstrap`) cuesta ~3× una compilación normal (en un i7-12700KF con KVM: GCC completo ≈ 20–40 min a -j8 en Linux; **dentro de AuroraOS contar 2–5× más** por el coste de `fork`/FS, todo **[por medir]**). Para el día a día: `--disable-bootstrap` y un solo `make`. El bootstrap completo se hace en el hito de validación, no en cada CI.
- Reproducibilidad: `SOURCE_DATE_EPOCH`, `-ffile-prefix-map`, `-fdebug-prefix-map`, rutas de build idénticas (`/work/build`) en host y AuroraOS, `LC_ALL=C`, y `--disable-nls`. Sin eso la comparación binaria (§8) falla por rutas y fechas.

---

## 6. Fases e hitos

Tallas: S = horas–1 día, M = varios días, L = 1–3 semanas, XL = más. Cada fase termina con una **prueba objetiva** y un commit.

### G0 — Preparación del sistema base (S–M)

- G0.1 Auditoría de syscalls: ejecutar en AuroraOS `strace`-equivalente (no hay `ptrace`; usar los logs `[SYSCALL-GAP]`/`[SYSCALL-ERR]` del kernel) con `bash`, `tar`, `sed`, `find`, `install`, `cp -a`, `mv`, `ln -s`, `touch -d`, `make --version`. Confirmar `renameat2`, `fdatasync`, `copy_file_range`.
- G0.2 **Conflicto de cabeceras:** `sysroot/usr/include` hoy es de musl (para `tcc`). Mover las de tcc a `/opt/tcc-musl/include` o `/usr/lib/musl/include` y configurar `tcc` para usarlas (`-I`/`CONFIG_TCC_SYSINCLUDEPATHS`); `/usr/include` será glibc. Si no, `gcc` verá `stddef.h` de musl y todo se rompe.
- G0.3 Subir los límites sin discusión: K4, K5, K6 (según D3). `make` obliga a revisar `/proc/sys/kernel/*` mínimos si alguno falta.
- G0.4 **Prueba:** `uname -a`, `nproc`, `getconf PATH_MAX`, `ulimit -a`, `df -h` coherentes; suite smoke de `glibc_port.md` §5 sin regresiones.

### G1 — Medir antes de construir (S)

- G1.1 Micro-tests de userspace propios (C, ya compilados con el toolchain del host): `mmap` de fichero (`MAP_PRIVATE`, `MAP_SHARED` lectura, `MAP_FIXED` sobre región ya mapeada), `ftruncate` + escritura dispersa, `rename` sobre existente, `symlink`/`readlink`, `link`, `utimensat` con ns, `fsync`, `flock`, `pipe` grande (> 64 KiB), 1 000 `fork+exec` encadenados, 100 000 ficheros pequeños en un directorio, nombres de 255 bytes, ruta de 4000 bytes.
- G1.2 **Benchmarks base** dentro de QEMU+KVM y en el portátil: `fork+exec` de `/bin/true` ×10 000, `bash -c 'for i in …'`, `tar x` de un tarball de 100 MB, `sed`/`grep` sobre 100 MB, `sort` de 50 MB. Guardar números en `docs/` para comparar tras cada mejora.
- G1.3 Prueba de presión de memoria: reservar y tocar el 90 % de RAM; comprobar que el kernel falla con `ENOMEM`/OOM ordenado y no con cuelgue.
- **Salida:** informe `docs/Bench_Toolchain_Base.md` con lista de fallos; cada fallo es un ticket K*.

### G2 — Primer `gcc` (hello world) sobre FAT32 / tmpfs (M)

Hito **H1**: `gcc hello.c -o hello && ./hello` dentro de AuroraOS.

- G2.1 `scripts/fetch-devtools.sh` (Escalón A): baja los `.deb` de Ubuntu (`gcc-13`, `cpp-13`, `binutils`, `libc6-dev`, `linux-libc-dev`, `libgcc-13-dev`, `make`, y deps), extrae a `build/devtools/` con `dpkg -x`, **resuelve los symlinks copiando** (para FAT32) y genera `aurora-devtools.img` (imagen de disco para QEMU; FAT32 en este escalón, ext2 más adelante).
- G2.2 Makefile: `make devtools-img`; `make run-devtools` (QEMU con la ISO + disco de herramientas + disco de trabajo de 16 GB + `-m 4G`).
- G2.3 Arranque: `mount` del disco en `/opt/devtools`, `PATH`, `LIBRARY_PATH`, `C_INCLUDE_PATH`, `GCC_EXEC_PREFIX` según haga falta. Resolver la ruta de `/lib64/ld-linux-x86-64.so.2` (ya existe en la ISO).
- G2.4 **Pruebas:** `gcc -v`; `gcc -c hello.c`; `as`/`ld` aislados; `gcc -static`; `gcc -O2 -shared -fPIC`; `g++ hello.cc`; `ar rcs libx.a …`; `make` con un `Makefile` de 3 ficheros. Cada fallo es un bug de kernel (anotarlo).
- Riesgo: sin symlinks en FAT32 se rompen nombres de soname (copias duplicadas); aceptable aquí, no más allá.

### G3 — Entorno de build real sobre ext2 (M, depende de `Plan_Ext2` E0–E3)

Hito **H2**: compilador nativo estable. Sustituye el disco de herramientas por ext2 (symlinks, hard links, modos) y el disco de trabajo `/work` por ext2.

- G3.1 `mke2fs -t ext2 -I 256` en el host; `fetch-devtools.sh` genera la imagen con `debugfs`/`mke2fs -d` (sin root).
- G3.2 Tests de `autoconf`: `sh configure` de un paquete pequeño (`zlib`, `make`, `sed`) con salida a `/work`. Debe pasar sin `config.guess` manual (config.sub/guess con `aurora`, §2.2).
- G3.3 **Compilar `make` 4.4 con el gcc nativo**, instalar en `/usr/local`, usarlo para compilar un segundo `make` (Hito **H3**). Compararlo contra el compilado en el host (§8).
- G3.4 Ídem `gzip`, `sed`, `tar`, `bash` (H3 ampliado): sirven de test de regresión del kernel y se añaden a la smoke suite.

### G4 — Binutils desde fuentes en AuroraOS (M)

Hito **H4**: `binutils-2.42` compilado y reinstalado dentro de AuroraOS (`configure --prefix=/opt/aurora-tc --disable-nls --disable-gprofng --disable-werror`). Valida `make -j` con muchos procesos, `ld` y `ar` (rename + mmap), `makeinfo` desactivado. Prueba: `ld` nuevo enlaza `hello`; `objdump -d` coincide con el del host byte a byte.

### G5 — GCC desde fuentes en AuroraOS (L)

Hito **H5**: GCC 13.3.0 compilado dentro de AuroraOS con el toolchain de Ubuntu (stage1).

- G5.1 Fuentes en `/work/src` (tarball + GMP/MPFR/MPC/ISL descomprimidos en el árbol con `contrib/download_prerequisites` previamente ejecutado en el host).
- G5.2 `configure`: `--prefix=/opt/aurora-tc --build=… --host=… --target=… --enable-languages=c,c++ --disable-multilib --disable-nls --disable-bootstrap --disable-libsanitizer --disable-libvtv --disable-lto --with-system-zlib --with-sysroot=/ --disable-libquadmath`. Primero **`--disable-bootstrap` y `-j1`** (fácil de depurar), luego `-jN`.
- G5.3 Observar: pico de memoria, tiempo, procesos fallidos (`ENOMEM`, `EAGAIN` en `fork`, `EMFILE`). Ajustar kernel (K5, COW si procede).
- G5.4 **Prueba:** `/opt/aurora-tc/bin/gcc` compila `hello`, `make` y el propio `binutils`; tests de GCC `make check-gcc RUNTESTFLAGS="dg.exp=…"` con un subconjunto (necesita `dejagnu`, `expect`, `tcl`: opcional; si no, 5–10 programas de prueba propios).

### G6 — Bootstrap completo y comparación (M)

Hito **H6**: `make bootstrap` (3 etapas) con `--enable-bootstrap` termina y `stage2` = `stage3`. Es el test de estrés definitivo (miles de procesos, ~10 GB de disco, RAM alta). Cualquier fallo no determinista aquí es un bug del kernel (race en `mmap`, `futex`, `pipe`, `wait`), y es valioso. Dejar documentado el tiempo total.

### G7 — Compilar el kernel y el userland de AuroraOS dentro de AuroraOS (M)

Hito **H7**: `make kernel` y `make user` dentro de AuroraOS producen un `kernel.elf` **idéntico** (o con diferencias explicadas) al del host.

- G7.1 Llevar `nasm` (portar desde fuentes 2.16, C puro) o usar el de Ubuntu del Escalón A. Comprobar que los `.asm` (`boot.asm`, `isr_stubs.asm`, `switch.asm`…) producen los mismos objetos.
- G7.2 Preparar el árbol de código: `git clone` en `/work/aurora` (las carpetas `third_party/`, `toolchain/`, `precompiled/` y `libs/glibc/` suman ~700 MB; el clon para el self-hosting debe poder prescindir de las pesadas o traerlas como artefactos aparte). Definir un **modo "mínimo"** del Makefile (`SELFHOST=1`): `WITH_NVIDIA=0`, `WITH_LKPI=0`/`WITH_MESA=0` (`setup-env.sh` ya admite `WITH_LKPI=0`), `SKIP_ENV=1` (**es obligatorio**: `setup-env.sh` hoy intenta `apt-get`), y usar los artefactos binarios de `precompiled/` y `libs/glibc/` como entradas sin reconstruir.
- G7.3 Adaptar los pasos del Makefile que asumen herramientas del host: `scripts/*.py` con `PIL`, `rsvg-convert`, `clang`, `sudo apt`, `/usr/share/OVMF`. Los fondos/iconos se **commitean ya generados** (o se llevan en `precompiled/`).
- G7.4 `-Wl,--build-id=none`, `SOURCE_DATE_EPOCH` y rutas fijas en `kernel/Makefile` para builds reproducibles. Hoy `-g -O0` en el kernel (`kernel/Makefile`) añade rutas absolutas de debug; usar `-ffile-prefix-map=$(CURDIR)=.`.
- **Prueba:** tiempo de `make -C kernel` en AuroraOS vs host; `cmp kernel.elf`; arrancar el kernel recién compilado en QEMU desde el propio AuroraOS (`aurora.iso` nueva con `initrd` del host, sin bootloader nuevo).

### G8 — Generar la ISO desde AuroraOS (M–L)

Hito **H8**: `make iso` dentro de AuroraOS produce `aurora.iso` que arranca en QEMU (UEFI y BIOS) y en el portátil.

- G8.1 Portar/llevar `mtools`, `dosfstools`, `xorriso`, `gdisk`: cada uno con su `configure`, todos C.
- G8.2 Escritura de imágenes grandes: `dd`, `mkfs.fat` sobre un fichero de ~1 GB, `xorriso` escribiendo ~1 GB, `mcopy` de un `initrd.tar` de 500 MB. Ejercita `ftruncate`, escritura dispersa, `lseek` > 2 GiB si procede, y E/S secuencial grande sobre ext2.
- G8.3 **Bootloader UEFI sin clang:** ver 6.4.
- G8.4 `make iso` completo con el initrd mínimo (sin Mesa/NVIDIA) y con el initrd completo tomando `precompiled/` como entrada. Dos ISOs: *mínima* (autocompilada entera) y *completa* (con binarios de terceros precompilados).

### G9 — Mejoras de rendimiento y limpieza (L, opcional, intercalable)

COW en `fork`; `readahead`/`writeback` de ext2; `tmpfs` dinámico; `sched_getaffinity`; `ccache` (si el reloj es estable); retirar `toolchain/x86_64-linux-musl-cross` y `tcc` o llevarlos a `/opt`; documentar todo en `docs/` y actualizar `glibc_port.md`/`TODO.md`/`ROADMAP.md` (ya marcan "GCC nativo" como P3).

### 6.4 El problema del bootloader UEFI (clang/PE)

`bootloader/efi` se compila con `clang -target x86_64-windows` + `lld-link`. En AuroraOS no hay LLVM. Opciones:

| Opción | Pros | Contras |
|---|---|---|
| (a) **Precompilar `BOOTX64.EFI` en el host y commitearlo** como artefacto (`bootloader/efi/BOOTX64.EFI` ya se usa tal cual en el `iso_root`) | Cero trabajo; el bootloader cambia poco. Se puede verificar con `sha256sum` | No es "100 % autocompilado"; hay que reproducir desde el host cada vez que cambie |
| (b) Compilarlo con GCC como ELF + `objcopy` a PE (patrón `gnu-efi` clásico: `-fpic -fshort-wchar -mno-red-zone`, `-T elf_x86_64_efi.lds`, `objcopy -O pei-x86-64`) | Autocompilado con binutils; `gnu-efi` ya está en `third_party/` | Reescribir el Makefile del bootloader y probar en firmware real (riesgo de no arrancar en el portátil Toshiba) |
| (c) Llevar `clang`+`lld` a AuroraOS | | LLVM es gigante, necesita C++ y 8+ GB: solo tras H6 |

**Recomendación:** (a) para H8 y (b) como hito posterior (H9) porque no bloquea nada. Aún así, (b) es la salida limpia.

---

## 7. Cadena de proveedores: qué se reconstruye y qué no

| Componente | En el self-hosting |
|---|---|
| kernel base (`kernel/*.c`, `.asm`) | **se compila** |
| userland propio (`user/`, `user/musl` si sigue) | **se compila** (musl con la toolchain `x86_64-linux-musl-cross` que habría que llevar, o se migra a glibc: D6) |
| drivers `.kur/.aur` (no LinuxKPI) | **se compila** (`tools/kurbuild`, `aurbuild`) |
| LinuxKPI (`linuxkpi.kur`), `nvidia*.kur` | precompilado (necesitan árbol de Linux y open-gpu-kernel-modules) |
| bootloader BIOS | **se compila** (nasm + gcc -m32 freestanding) |
| bootloader UEFI | precompilado (6.4a) |
| bash/coreutils/…, perl, python, git, openssl, curl, nano, tmux | precompilado (`precompiled/`, `libs/glibc/`) |
| Wayland/Weston/foot/GTK/Qt/Xwayland/Mesa/Vulkan | precompilado (descargas de `.deb`) |
| glibc | **precompilada** (2.39 de Ubuntu). Recompilar glibc es otro proyecto (M–L: necesita `bison`, `python3`, `gawk`, cabeceras Linux, `--enable-kernel=…`); no es requisito del self-hosting |
| toolchain (gcc, binutils, make) | **se compila** (H5/H6) |

Para que la ISO "completa" sea reproducible sin red, el Makefile ya guarda los binarios en `precompiled/` y `libs/glibc/`; el self-hosting solo los trata como entradas.

---

## 8. Pruebas y reproducibilidad

1. **Smoke suite del compilador** (`tests/gcc/`): `hello.c`, `hello.cc`, `-static`, `-shared`, `-fPIC`, `-O0/-O2/-O3`, `-flto` (cuando haya), `-pipe`, `-save-temps`, `-fstack-protector`, `-fsanitize` (no), `-pthread` + 20 hilos, `setjmp/longjmp`, `__thread`, `ifunc` (memcpy de glibc), `-march=native`. Cada ejecutable se compara con la salida esperada.
2. **Comparación de binarios con el host:** misma versión de GCC (13.3) y mismos flags en el host y en AuroraOS. Se comparan `cc1`, `as`, `ld` por `cmp`, y para los objetos de prueba `cmp a.o b.o` (los `.o` de ELF son deterministas sin fechas). Diferencias legítimas: rutas absolutas en `.comment`/`DW_AT_name` (usar `-ffile-prefix-map`), `build-id` (desactivar con `--build-id=none`), y orden de ficheros del `tar`. Un diff real es un bug de kernel o de toolchain y se investiga.
3. **Bootstrap-compare** de GCC (G6) como oráculo interno.
4. **QEMU en CI:** nuevo target `make test-selfhost` que arranca la ISO con `aurora-devtools.img` y `work.img`, ejecuta un script `selfhost-test.sh` por consola serie/`qemu-drive.py` (ya existe `scripts/qemu-drive.py`, `scripts/qemu-boot-test.sh`) y falla si algún paso devuelve ≠ 0; guarda log. Dos perfiles: *rápido* (hello, make, binutils: ~15 min) y *completo* (GCC + kernel + ISO: horas, solo manual/nocturno).
5. **Regresión del kernel:** cada bug de kernel encontrado por el self-hosting entra como test en `kernel/tests/` o `user/`, no solo como arreglo. El propio self-hosting es la mejor prueba de estrés (miles de procesos, ficheros, mmap).
6. **Hardware real:** repetir G2.4, G3.3 y G7 en el portátil Toshiba y en el PC solo tras pasar QEMU; ahí se mide rendimiento real (disco, USB). (Recordatorio de memoria: el PC es de pruebas delicadas; el portátil es el banco real.)
7. **Seguridad de los datos:** el disco de trabajo se monta aparte (`/work`); `make` jamás escribe en `/`. Copia de seguridad de la imagen antes de cada hito grande.

---

## 9. Riesgos

| Riesgo | Prob. | Impacto | Mitigación |
|---|---|---|---|
| `VFS_PATH_MAX`=128 sigue siendo el límite (rutas largas) | alta | bloqueante | K1 antes de G3; en G2 usar árboles cortos (`/w`) |
| `rename` sin sobrescritura | alta | bloqueante | K2 (comparte trabajo con ext2 E0) |
| FAT32 sin symlinks/hard links/modos | segura | bloqueante a partir de G3 | ext2 (Plan_Ext2 E3) |
| Rendimiento de `fork` sin COW | media | lentitud (×5–×20 en `configure`) | medir en G1.2; `vfork`/`posix_spawn`; COW (G9) |
| RAM insuficiente (initrd 500 MB en RAM + tmpfs + build) | media | cuelgues no deterministas | 8 GB para H5/H6, `TMPDIR` en disco, probar OOM en G1.3 |
| Bugs de concurrencia (futex, `mmap_race`, señales) que se manifiestan sólo con 10 000 procesos | media | fallos esporádicos del build | `make -j1` primero, luego -jN; cada fallo → test mínimo; revisar `ref_count` no atómico anotado en `TODO.md` |
| mtime con resolución de 1–2 s | media | `make` no reconstruye / reconstruye mal | ext2 inodos 256 B + `utimensat` ns; `touch` explícitos; `.NOTPARALLEL` temporal |
| `config.sub`/`config.guess` no conocen `aurora` en cada paquete | segura | `configure` aborta o cae a rama genérica | copia común de config.sub/guess + `scripts/aurora-autoreconf.sh`; tabla de excepciones (§2.3) |
| Conflicto de cabeceras musl/glibc en `/usr/include` | segura | `gcc` usa cabeceras equivocadas | G0.2 |
| `lto1`/bootstrap necesitan > 4 GB | media | OOM | `--disable-lto`, `-j1`/`-j2`, swap (si `swap.c` sirve) o más RAM |
| ext2 no listo a tiempo | media | G3+ bloqueados | G0–G2 progresan igual; el sidecar de DU1 **no** sirve para symlinks, no usarlo como puente aquí |
| Bootloader UEFI no se puede reconstruir sin clang | segura | H8 no 100 % autocompilado | 6.4(a) y luego (b) |
| Licencia/tamaño de fuentes (GCC+glibc) en el repo | baja | repo gigante | no commitear tarballs; `scripts/fetch-gcc-src.sh` los baja y verifica `sha256` |
| Dependencia de la red para bajar fuentes dentro de AuroraOS | media | | pre-descargar en el host al disco de herramientas; la red TCP/IP aún está en desarrollo (`plan_stack_tcp_ip_para_aurora_os.md`) |
| Divergencia respecto a Ubuntu (glibc 2.39 fija) | media | actualizaciones | fijar versiones; `scripts/fetch-devtools.sh` verifica hashes |

---

## 10. Resumen de hitos

| Hito | Fase | Prueba |
|---|---|---|
| **H0** | G0–G1 | Smoke suite sin regresiones; informe de benchmarks y lista de bugs K* |
| **H1** | G2 | `gcc hello.c && ./a.out` en AuroraOS (FAT32/tmpfs) |
| **H2** | G3 | Compilador nativo estable sobre ext2 (`g++`, `ar`, `make`, `-shared`, `-pthread`) |
| **H3** | G3 | `make`, `sed`, `gzip`, `tar`, `bash` compilados dentro de AuroraOS y su resultado equivale al del host |
| **H4** | G4 | binutils 2.42 compilado dentro, `objdump` idéntico |
| **H5** | G5 | GCC 13.3 compilado dentro con `--disable-bootstrap`, compila `hello` y binutils |
| **H6** | G6 | `make bootstrap` completo, stage2 = stage3 |
| **H7** | G7 | `kernel.elf` y `user/` compilados dentro = los del host (o diferencias explicadas); arranca |
| **H8** | G8 | `make iso` dentro de AuroraOS produce una `aurora.iso` que arranca en QEMU UEFI/BIOS y en el portátil |
| **H9** | 6.4(b), G9 | Bootloader UEFI con GCC; COW; retirada de musl-cross/tcc de la ISO base |

Orden recomendado y dependencias:

```
K1,K2,K4,K5,K6 ─► G0 ─► G1 ─► G2 (H1, sobre FAT32)
                                  │
Plan_Ext2 E0..E3 ─────────────────┴─► G3 (H2,H3) ─► G4 (H4) ─► G5 (H5) ─► G6 (H6)
                                                        │
                                                        └─► G7 (H7) ─► G8 (H8) ─► H9
```

Estimación gruesa (una persona + asistente, sin contar `Plan_Ext2`): G0–G2 ≈ 1–2 semanas; G3–G5 ≈ 3–5 semanas (casi todo depurar el kernel); G6–G8 ≈ 2–4 semanas. **[por medir]** tras G1.

---

## 11. Decisiones para Alvaro

| # | Decisión | Opciones | Recomendación |
|---|---|---|---|
| **D1** | Triplete | **Decidido por Alvaro:** propio, `x86_64-pc-aurora-gnu` (alternativa `x86_64-aurora`), con parches pequeños (§2.2) | Confirmar el nombre exacto; recomiendo `x86_64-pc-aurora-gnu`. |
| **D2** | Primer escalón | (a) usar `.deb` de Ubuntu (Escalón A) hasta H4 y construir el propio después; (b) construir desde el principio un toolchain propio en el host | **(a):** rápido y prueba el kernel de inmediato; (b) llega después como H5 igualmente. |
| **D3** | Macros predefinidas y `uname` | **Decidido por Alvaro (2026-11-01):** el compilador define `__aurora__` **y** `__linux__`/`__gnu_linux__`; no se modifican paquetes de terceros. `uname -s` sigue siendo "Aurora". | Cerrada. |
| **D4** | Dónde vive el toolchain | (a) en el initrd (+300 MB de RAM); (b) disco/partición aparte `aurora-devtools.img`; (c) tarball extraído en `/work` | **(b)**. El initrd ya es de 500 MB en RAM. |
| **D5** | Sistema de ficheros del disco de trabajo | (a) FAT32 (solo H1); (b) ext2 (`Plan_Ext2` E3); (c) esperar a ext4 | **(b)**; FAT32 sirve únicamente para H1. Hacer E0–E3 del plan ext2 **antes** de G3, y K1 (`VFS_PATH_MAX`) dentro de E0. |
| **D6** | Qué hacer con `user/musl` y `tcc` | (a) mantenerlos con el toolchain musl (llevarlo a la ISO de desarrollo); (b) migrarlos a glibc y retirar musl-cross | **(b)** a largo plazo (G9); (a) mientras tanto. El plan no necesita musl. |
| **D7** | Alcance del self-hosting | (a) mínima: kernel + user + drivers `.kur/.aur` + ISO con terceros precompilados; (b) además compilar Wayland/Weston/GTK; (c) todo (incl. Mesa/LLVM/Qt) | **(a)** para H8. (b)/(c) no son un proyecto de OS, son de packaging; se decidiría con un gestor de paquetes. |
| **D8** | Bootloader UEFI | 6.4 (a), (b) o (c) | **(a) ahora, (b) después (H9).** |
| **D9** | RAM de referencia del test de self-hosting | 4 GB / 8 GB / 16 GB | **8 GB** en QEMU; documentar el mínimo real tras G6. |
| **D10** | Cuándo empezar | (a) G0–G2 ya, en paralelo con ext2; (b) tras ext2 E3 | **(a):** G0–G2 no dependen de ext2 y generan la lista de bugs del kernel que ext2 también necesita (K1, K2). |

---

## 11b. Avance (2026-11-01)

- **K5 hecho:** `MAX_PROCESS_FDS` 256 → 1024 (`kernel/vfs.h`). Compila; sin probar en QEMU.
- **K4 hecho:** `/tmp` = 1/4 de la RAM entre 64 MB y 1 GB (`kernel/tmpfs.c`). Compila; sin probar en QEMU.
- **K6 (parte herramienta) hecho:** `scripts/aurora-config-sub.sh` parchea `config.sub`/`config.guess` para `x86_64-pc-aurora-gnu`; `--selftest` OK con el config.sub de Ubuntu.
- **G2.1 (parte descarga) hecho:** `scripts/fetch-devtools.sh` baja 49 .deb (gcc-13, g++-13, binutils, make, libc6-dev, libstdc++-13-dev, nasm, patch, m4 y dependencias) y extrae 242 MB a `build/devtools/root`; `--img` genera `build/aurora-devtools.img` (FAT32, symlinks copiados). La imagen no se ha probado en AuroraOS.
- **Pendiente de G0:** K1 (`VFS_PATH_MAX`, cambio grande y delicado), K2 (`rename` con sobrescritura, se coordina con `Plan_Ext2` E0), G0.2 (mover cabeceras musl de tcc), G0.1 (auditoría de syscalls en QEMU), `uname`/`os-release`.

### Avance 2 (Escalón B, cross en el host) — hecho y verificado en el host

- Decisión de Alvaro (tarjeta): **dos niveles**. Las herramientas se identifican como `x86_64-pc-aurora-gnu`; a los programas de terceros `config.guess` les dará `x86_64-pc-linux-gnu` (libtool de cada paquete solo conoce `linux*`; no se tocan terceros). El segundo nivel ya está en `aurora-config-sub.sh`: por defecto el `config.guess` parcheado devuelve linux-gnu; el toolchain propio usa `AURORA_GUESS=x86_64-pc-aurora-gnu`.
- `scripts/aurora-toolchain-patch.sh` + `toolchain-aurora/aurora.h`: parchea binutils 2.42 (4 ficheros de destino) y GCC 13.3 (`config.gcc`, `libgcc/config.host`, `libstdc++-v3/configure.host`, y los `configure` generados de los directorios de bibliotecas, donde se añade `aurora*` junto a `linux*`). Son ~24 ficheros tocados por sed/python, no unas pocas líneas: más de lo que estimé al principio.
- `scripts/mk-aurora-sysroot.sh` y `scripts/build-aurora-toolchain.sh` construyen sysroot y cross. **Probado** (en el host, 4 núcleos, ~35 min en total): `x86_64-pc-aurora-gnu-gcc -dumpmachine` → `x86_64-pc-aurora-gnu`; define `__aurora__`, `__AURORAOS__`, `__linux__` y `__gnu_linux__`; compila C y C++ (libstdc++) y enlaza dinámico (intérprete `/lib64/ld-linux-x86-64.so.2`), estático y `-shared`; los binarios se ejecutan correctamente en el host (misma ABI).
- Hallazgos: `libs/glibc` no trae `libmvec.so.1` ni `libutil.so.1` (el `libm.so` de glibc los referencia con `AS_NEEDED`); el sysroot los toma del host. Sin `makeinfo` hay que pasar `MAKEINFO=true`.
- **Actualización 2026-11-02:** `fork` con COW hecho en la rama `claude/project-thread-cxw685` (sin fusionar): fork de 192 MB de ~5 s a ~130 ms en QEMU; el bloqueo "fork sin COW" queda resuelto.
- **Aún no probado en AuroraOS:** ejecutar estos binarios en QEMU. Bloqueos para el siguiente paso: ISO/QEMU funcionando con la rama de ext2 fusionada (disco de trabajo con symlinks), `fork` sin COW (medir), y mmap `MAP_SHARED` escribible (E3).

### Avance 3 (2026-11-02): primera ejecución dentro de AuroraOS (QEMU, `tests/gcc/run_hello_test.sh`)

main fusionado (fork COW, ext2 E0-E4, usuarios). Imagen ext2 de 768 MB con el toolchain de Ubuntu montada en `/data`.

- **OK:** hello en C y C++ del cross (dinámicos) se ejecutan; ven `__aurora__`.
- **OK:** el gcc nativo de Ubuntu ejecuta la cadena completa cc1 → as → collect2 → ld y produce un ejecutable estático (`gcc -static`, rc=0).
- **Bug de kernel 1 (corregido):** `execve` truncaba en silencio argv a 16 entradas y envp a 64 de 256 B (gcc→cc1/ld llegaban cortados: "missing filename after -o"). Ahora pool de heap de 256 KiB, hasta 256 argumentos y 256 variables, `E2BIG` si se supera (`kernel/syscall.c`, `process.h`). Los caminos `spawn` mantienen sus límites antiguos.
- **Bug de kernel 2 (corregido):** los binarios **estáticos** de glibc imprimían bien y morían con `SIGSEGV` al salir. Causa: `execve` dejaba los registros del padre; `rdx` (= envp, puntero al heap) es el `rtld_fini` que el ABI manda registrar con `atexit` en `_start`, y glibc estática lo llamaba al salir (salto a heap NX). Ahora el programa nuevo arranca con los registros a cero, como Linux. Verificado en QEMU: `static`, `static-pie` y estático del cross, rc=0. Además `pf.c` registra ahora las violaciones de permisos de usuario (pid, rip, cr2, err), que antes eran silenciosas.
- **Cuello de botella 3 (abierto): lectura de ext2 muy lenta.** Leer `cc1` (~30 MB) tarda ~41 s (≈0,7 MB/s) y cada compilación 1–2 min de reloj con CPU casi nula: una E/S por bloque de 4 KiB, sin lectura anticipada. Un bootstrap de GCC no es viable así; hace falta readahead/E/S multibloque en el driver ext2 y/o en AHCI.
- La prueba dinámica nativa (`gcc hello.c`) se probará cuando la lectura sea razonable; ya está preparada con el runtime de la ISO en el sysroot de `/data`.

### Avance 4 (2026-11-04): compilar make dentro de AuroraOS (`tests/gcc/run_make_test.sh`)

- **Bug de kernel 4 (corregido, grave):** `k_execve` tomaba un *spinlock con IRQs desactivadas* (`g_execve_lock`) y dentro leía el ELF del disco (ext2 → driver AHCI de usuario), es decir, dormía. Con dos CPUs ejecutando `execve` a la vez, una giraba sin interrupciones y el dueño del cerrojo no podía despertar: **cuelgue total** (ambas CPUs en `spin_lock_irqsave`, visto con el monitor de QEMU a los ~3 min de `./configure`). Ahora es un mutex que duerme.
- **Bug de kernel 5 (corregido):** `fchmodat`, `fchownat` y `utimensat` devolvían EINVAL con dirfd real + ruta relativa (GNU tar/gnulib), y faltaba la syscall `fchmodat2` (452) que glibc 2.39 usa con `AT_SYMLINK_NOFOLLOW`. Ahora `tar xzf` de make-4.4.1 en ext2 termina con rc=0 y sin errores.
- **Rendimiento:** con TCG (sin KVM) `./configure` de make avanza ~90 comprobaciones por hora de reloj del guest (cada `gcc` de prueba tarda ~20 s del guest): inviable aquí; con KVM debería ser decenas de veces más rápido. Sospecha: la caché de bloques no retiene `cc1`/`as`/`ld` (~50 MB) entre ejecuciones y se vuelven a leer cada vez; a medir en el hilo de ext2.
- **Infraestructura de pruebas (no corregido):** el ejecutor `/system/boot-tests` de `init` usa `waitpid` sin reintentar en `EINTR`, por lo que órdenes largas se solapan con la siguiente; el test lo evita lanzando la compilación en segundo plano y esperando con un bucle. Además `toolchain/x86_64-linux-musl-cross` de este entorno no trae `crt1.o`, así que `user/musl` no se puede recompilar aquí (el binario `user/musl/init` está versionado).

## 12. Primeros pasos concretos (si Alvaro dice "adelante")

1. G0.1–G0.4 (limpiar cabeceras de `/usr/include`, subir límites, tests de smoke).
2. G1 completo (micro-tests y benchmarks); publicar `docs/Bench_Toolchain_Base.md`.
3. `scripts/fetch-devtools.sh` + `make devtools-img` + `make run-devtools`.
4. H1 (hello world).
5. Con K1/K2 hechos (puede coordinarse con la fase E0 de `Plan_Ext2`): retomar G3 en cuanto ext2 E3 esté disponible.

**Qué no se hace sin confirmación expresa:** modificar el sistema de build de la ISO existente (`Makefile`) más allá de añadir targets nuevos con prefijo `devtools-`/`selfhost-`, tocar el bootloader, o empezar LinuxKPI L8/L9.

## Avance 5 (2026-11-04): herramientas de empaquetado y modo SELFHOST

- `scripts/fetch-devtools.sh` ahora baja también mtools, dosfstools, xorriso, fdisk, gdisk, xxd, gawk, bison, flex, bc (nasm y m4 ya estaban), aplana /sbin y /bin dentro de /usr y copia los módulos gconv del host (mtools convierte a codepage 850 con iconv; sin ellos `mmd/mcopy` fallan con "Error setting code page").
- `tests/gcc/run_tools_test.sh` (QEMU): nasm, mkfs.fat, mmd, mcopy, mdir, xorriso, xxd, gawk, sfdisk, sgdisk, m4, bison y flex funcionan dentro de AuroraOS. Para mtools hace falta `GCONV_PATH=/usr/lib/x86_64-linux-gnu/gconv` (o los gconv en la ISO).
- `make SELFHOST=1 ...`: SKIP_ENV, WITH_NVIDIA=0, WITH_MESA=0 y no recompila user/musl (se usan los binarios versionados).
- Limitación conocida del kernel: un fichero de /tmp (tmpfs) no puede crecer por encima de ~32 MB (krealloc duplica la capacidad y falla a 64 MB: `dd` da ENOMEM). Imágenes grandes (ISO, initrd.tar) deben escribirse en /data (ext2) hasta que tmpfs use páginas en lugar de un bloque contiguo.

## Avance 6 (2026-11-04): herramientas en la ISO y /tmp grande

- `make iso` (WITH_DEVTOOLS=1 por defecto) instala en la ISO gcc/g++ 13, binutils, make, nasm, mtools (+gconv), dosfstools, xorriso, fdisk/gdisk, xxd, gawk, bison, flex, m4, bc, patch y las cabeceras/libc.a de glibc. WITH_DEVTOOLS=0 las omite. La ISO pasa de ~650 MB a ~920 MB. Las cabeceras y libc.a de glibc sustituyen a las de musl del tcc (tcc queda en desuso).
- `scripts/fetch-devtools.sh` ya no usa apt del host: `scripts/ubuntu-fetch-debs.py` resuelve y baja los .deb directamente del archivo de Ubuntu 24.04, así que en Kali se obtiene la misma glibc 2.39 que la ISO.
- tmpfs guarda los ficheros en páginas del PMM (tabla de punteros), no en un krealloc contiguo del heap (límite 64 MB/bloque, 256 MB total). /tmp = 1/2 de la RAM entre 64 MB y 2 GB, las páginas se piden al escribir. Con QEMU 2 GB: /tmp 586 MB y `dd` de 300 MB OK.
- Verificado en QEMU con la ISO: `tests/gcc/run_iso_tools_test.sh` (todas las herramientas en PATH, gcc compila y ejecuta hello, fichero de 300 MB en /tmp, mkfs.fat+mmd+mcopy+mdir); tests del kernel 211/211.

## Avance 7 (2026-11-04): ISO completa por defecto

- Por defecto `make iso` incluye: LinuxKPI (i915, drm, ttm, virtio-gpu...), NVIDIA (WITH_NVIDIA=1), Mesa/LLVM (WITH_MESA=1, `fetch-mesa.sh` ya no usa apt del host), DEV TOOLS (WITH_DEVTOOLS=1), /data en ext2 (USB_DATA_FS=ext2) y el código fuente del repo en `/data/AuroraOS` (WITH_SOURCE=1; solo ficheros versionados, sin .git; partición de 3 GB, `SRC_DATA_MB`). La ISO pasa a ~4,5 GB. Desactivar: `WITH_MESA=0 WITH_NVIDIA=0 WITH_DEVTOOLS=0 WITH_SOURCE=0 USB_DATA_FS=fat32`. QEMU_MEM por defecto 3G.
- /data con las fuentes solo se monta al arrancar la ISO como USB (`make run-iso-as-usb`); con `-cdrom` /data es el tarfs.
- Red en QEMU: DHCP, ARP y el ping a la puerta (10.0.2.2) funcionan. `ping 8.8.8.8` da 100 % de pérdida con `-nic user` si el host no permite sockets ICMP a QEMU: `sudo sysctl -w net.ipv4.ping_group_range="0 2147483647"`. Probar con TCP/DNS (curl). Bug real corregido: DHCP escribía /etc/resolv.conf sin truncar y dejaba "ameserver 8.8.8.8" (ahora DNS del DHCP + 8.8.8.8).
