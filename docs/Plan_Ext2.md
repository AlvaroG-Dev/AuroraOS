# Plan_Ext2 — ext2 completo en AuroraOS

Estado (2026-11-01): **solo plan, sin código.** Rama `claude/plan-ext2` (desde `main`). Tallas: S ≈ una sesión, M ≈ 2–3, L ≈ 4+. Lo que aquí se afirma del código se leyó en `main` (`81660bc2`); donde algo no se verificó se dice.

Relacionado: `Plan_Usuarios.md` (rama `claude/plan-usuarios`, U2 y DU1), `Plan_Power.md` (apagado), `docs/Log_Persistente.md` (log en `/data`). La libc es **glibc**; no se propone busybox ni musl.

---

## 0. Objetivo y alcance

Un driver de kernel **ext2 de lectura y escritura**, fiel al formato de disco de Linux (rev 0 y rev 1), de modo que:

1. Un disco/imagen creado con `mke2fs` del host se monte en AuroraOS y viceversa (`e2fsck` del host debe dar 0 tras nuestras escrituras).
2. `/data` (y más adelante `/`) tenga **uid/gid/modo reales, enlaces duros y simbólicos, tiempos, sticky/setuid**, sin ficheros de metadatos auxiliares.
3. Haya herramientas de usuario (`mke2fs`, `e2fsck`, `debugfs`, `tune2fs`, `dumpe2fs`) para formatear, comprobar y reparar desde el propio sistema.
4. Se pueda instalar y arrancar AuroraOS con raíz en ext2.

**Dentro:** ext2 rev 0 y rev 1, bloques de 1024/2048/4096, features `filetype`, `sparse_super`, `large_file`, `ext_attr` (ignorar contenido), `resize_inode` y `dir_index` (compatibles, ver §2.3), inodos de 128 o 256 bytes.
**Fuera (se rechaza el montaje con un mensaje claro):** journal (`has_journal`, `needs_recovery`), extents, `64bit`, `flex_bg`, `metadata_csum`, `meta_bg`, `compression`, bloques >4096. Todo eso es ext3/ext4; ver fase E8 (opcional, solo lectura).

---

## 1. Lo que hay hoy (verificado en el código)

| Pieza | Estado | Consecuencia para ext2 |
|---|---|---|
| **VFS** (`kernel/vfs.c`, `vfs.h`) | `vfs_fs_ops_t` solo tiene `lookup(fs_priv, path_relativo)`, `name` y `statfs`. Las operaciones por nodo (`vfs_ops_t`) ya cubren `read/write/create/mkdir/unlink/mknod/truncate/readdir/rename/readlink/symlink/chmod/chown/link/fsync/utimes/get_page/poll/ioctl`. Comentarios del propio `vfs.h` ya anuncian "ext2" para `link` y `chmod`. | La interfaz de operaciones **ya existe**; ext2 es sobre todo implementar `vfs_ops_t`. |
| Resolución | **Por ruta**: cada `vfs_lookup` llama a `fs->lookup(priv, "/a/b/c")` y recibe un `vfs_node_t` recién `kmalloc`ado. No hay dcache ni caché de inodos; `node->name` guarda la ruta completa (128 bytes). | Dos `open()` del mismo fichero obtienen nodos distintos: el estado del inodo (tamaño, bloques, dirty) **no puede vivir en el nodo**. Hace falta una **caché de inodos por FS** (§3.2). |
| Límites | `VFS_PATH_MAX = 128` (ruta completa y `vfs_dirent_t.name`); `node->inode` es `uint32_t`; `vfs_stat_t` solo trae `mtime` (sin `nlink`, `atime`, `ctime`, `blocks`). | ext2 permite nombres de 255 bytes y `st_nlink/atime/ctime/blocks` reales. Ver §3.1 y decisión D5. |
| `rename` | El comentario de `vfs.h` decía que no se sobrescribía, pero era **obsoleto**: `vfs_rename` delega en el FS y tmpfs y FAT32 ya sobrescriben (comprobado al empezar E0.1; comentario corregido). | ext2 debe implementar la sobrescritura atómica en su propio `rename` (patrón `fichero.tmp` + `rename` de `passwd`/`shadow`). No hace falta cambio de VFS. |
| **Montaje** | **No existe `mount(2)` ni `umount(2)`** (no hay entrada en la tabla de syscalls). Los montajes los hace el kernel en `kernel/main.c` (`try_mount_data` → `fat32_mount_bdev(..., "/data")`; `iso9660_mount_bdev("/cdrom")`). Hay `vfs_mount/vfs_umount/vfs_mount_bind/vfs_pivot_root`. Sin opciones (ro/rw, nosuid…). En `precompiled/usr/bin` ya están `mount`/`umount` de util-linux. | Hace falta `mount(2)`/`umount2(2)` con `MS_*` y una tabla de opciones por montaje (compartida con U1.4/U2.2 de usuarios, ver §8). |
| **Capa de bloques** (`block.c/h`, `part.c`) | Dispositivos con `sector_size` (512 en ATA/AHCI/USB; 2048 en CD), `bdev_read/bdev_write/bdev_flush`, E/S síncrona de cara al FS, particiones MBR y GPT con nombres `sda1`… Drivers: ATA PIO/DMA, AHCI (`kernel/ahci.c` y `drivers/aur/ahci`), USB BOT/UAS (`kernel/usb/usb_storage.c`), proxy a drivers de usuario (`block_proxy.c`). **No hay driver NVMe** (solo se nombra la clase PCI). | ext2 va por `bdev_*` y funciona en AHCI/USB/ATA ya. NVMe es un plan aparte; no bloquea. |
| **Caché de bloques** | **No hay una genérica.** FAT32 tiene la suya privada: 256 slots × 512 B (`fat32.c`, "[B] Buffer cache"), con *sync diferido* (4 MB o 1 s). Bloques de 512 B, alineación especial por el BMIDE. | ext2 necesita una caché de bloques propia y reutilizable (§3.3). No se toca la de FAT32. |
| **Sincronización** | `k_sync` devuelve 0 sin hacer nada (el comentario dice "FAT32 sincroniza en cada write", pero en realidad es diferido). `power.c` llama solo a `fat32_sync_all()`. `fsync` existe por nodo. | Registro genérico de "FS con caché sucia" para `sync`, `syncfs`, apagado y `umount` (§3.4). Sin esto, ext2 perdería datos en `poweroff`. |
| **Dispositivos de bloque en `/dev`** | `vfs.c` ya expone `/dev/<bdev>` con lectura/escritura de bytes arbitrarios (bounce buffer) y los ioctl `BLKGETSIZE`, `BLKGETSIZE64`, `BLKSSZGET`. | `mke2fs`/`e2fsck` en espacio de usuario pueden trabajar sobre ellos. Faltan `BLKFLSBUF`, `BLKROGET`, y **coherencia con la caché del kernel** si el FS está montado (§6.2). |
| **FAT32 como modelo** | `fat32.c` (≈4000 líneas) con `fat32_mount/umount/sync_all/check`, `ASYS_FS_CHECK` (`aurora-fsck` en `user/musl/apps/aurora-fsck`), `statfs`, `utimes`, `get_page` no. `chown` es no-op, `uid=gid=0` fijo, modo solo en RAM. | Plantilla de estructura (mount/umount/statfs/check) y la razón de ser de este plan. |
| **tmpfs** (`tmpfs.c`) | Modelo en RAM con `mode/uid/gid/nlink/vfs_refs`, hard links, symlinks, sticky y propietario = `fsuid` (tras U1). | Referencia de **semántica POSIX** que ext2 debe igualar (tests compartidos, §7). |
| **Arranque/ISO** | El `Makefile` añade al final de la imagen una partición MBR 2 FAT32 `AURORADATA` (`USB_DATA_MB=64`, `mkfs.fat`) que el kernel monta en `/data`. `mount_boot_media()` prueba FAT32 en cada disco/partición. Existe `bootloader/aurora.conf`. `vfs_pivot_root` ya existe. | ext2 entra como segundo tipo de FS en `mount_boot_media` (sondeo por magia, no por tipo de partición) y el Makefile gana `USB_DATA_FS=fat32|ext2`. |
| **Usuarios** | U0/U1 hechos (fsuid, sticky, setuid). U2 pendiente: DU1 recomienda *sidecar* `/data/.aurora-meta` ahora y "driver ext2 después, en plan aparte" (U2.5 "esbozo" deja el punto de montaje preparado). | Este documento **es** ese plan aparte; la relación exacta está en §8. |

Herramientas del host disponibles para las pruebas (en el entorno de desarrollo se comprobó): `mke2fs`, `e2fsck`, `debugfs`, `dumpe2fs` 1.47.0. Con `mke2fs -t ext2 -r 0` sale rev 0 sin features; con `-r 1` y el `mke2fs.conf` por defecto salen `ext_attr dir_index filetype sparse_super large_file` e inodos de 256 bytes.

---

## 2. Formato en disco que hay que implementar

### 2.1 Superbloque (offset 1024 desde el inicio del volumen, 1024 bytes)

Campos usados: `s_inodes_count`, `s_blocks_count`, `s_r_blocks_count`, `s_free_blocks_count`, `s_free_inodes_count`, `s_first_data_block` (1 con bloques de 1 KiB, 0 con >1 KiB), `s_log_block_size` (`1024 << n`), `s_blocks_per_group`, `s_inodes_per_group`, `s_mtime/s_wtime`, `s_mnt_count/s_max_mnt_count`, `s_magic = 0xEF53`, `s_state` (VALID_FS=1, ERROR_FS=2), `s_errors` (continuar / remount-ro / panic), `s_lastcheck`, `s_checkinterval`, `s_rev_level`. En rev 1: `s_first_ino` (11 por defecto), `s_inode_size`, `s_feature_compat/incompat/ro_compat`, `s_uuid`, `s_volume_name`, `s_last_mounted`, `s_last_orphan`.

**Reglas de validación al montar** (todas por defecto a "rechazar con motivo en el log", nunca confiar en el disco):
`s_magic`; `s_log_block_size` ≤ 2 (4 KiB); `s_blocks_per_group` ≤ 8·bloque; `s_inodes_per_group` ≤ 8·bloque y múltiplo de inodos por bloque; `s_blocks_count ≤ tamaño del dispositivo / bloque`; `s_inode_size` potencia de 2 entre 128 y el bloque; nº de grupos = ⌈(blocks−first_data)/blocks_per_group⌉ y coherente con `inodes_count`; tabla de descriptores cabe en el disco.

### 2.2 Features

| Máscara | Bit | Tratamiento |
|---|---|---|
| compat | `dir_prealloc`, `imagic_inodes`, `ext_attr`, `resize_inode`, `dir_index`, `has_journal`(compat) | `ext_attr`, `resize_inode`, `dir_index`: **aceptar**. `has_journal` presente **y** `needs_recovery` (incompat) activo ⇒ rechazar rw y ro ("fs con journal sin recuperar"). `has_journal` limpio ⇒ montar **solo ro** en la v1 (un ext3 limpio es legible como ext2). |
| incompat | `compression`, `filetype`, `recover`, `journal_dev`, `meta_bg`, `extents`, `64bit`, `mmp`, `flex_bg`, `ea_inode`, `dirdata`, `csum_seed`, `largedir`, `inline_data`, `encrypt`, `casefold` | Solo `filetype` aceptado. Cualquier otro bit ⇒ rechazar (`-EINVAL` + log con la máscara). |
| ro_compat | `sparse_super`, `large_file`, `huge_file`, `gdt_csum`, `dir_nlink`, `extra_isize`, `quota`, `bigalloc`, `metadata_csum`, `readonly` | `sparse_super` y `large_file` aceptados rw. Resto desconocido ⇒ **solo ro**. |

Rev 0: inodos de 128 bytes, `s_first_ino = 11`, sin features, backups del superbloque en **todos** los grupos (sin `sparse_super`).

### 2.3 Grupos, bitmaps, tabla de inodos

- Tabla de descriptores de grupo en el bloque siguiente al del superbloque: `bg_block_bitmap`, `bg_inode_bitmap`, `bg_inode_table`, contadores libres, `bg_used_dirs_count`. Con `resize_inode` hay GDT reservada: se **respeta y se salta** (no se usa ni se mueve).
- Copias de seguridad de superbloque + GDT en los grupos 0, 1 y potencias de 3, 5, 7 con `sparse_super`; en todos sin él. Se **escriben** (contadores, estado) solo en el grupo 0 en tiempo de ejecución, y se sincronizan todas en `umount` limpio — o se dejan a `e2fsck`, que es lo que hace Linux (decisión menor, ver E4).
- Bitmap de bloques e inodos: 1 bloque por grupo; los bits sobrantes del último grupo se ponen a 1 al crear.
- Inodo: `i_mode`, `i_uid` + `i_uid_high`, `i_size` + `i_dir_acl` (tamaño alto en ficheros regulares con `large_file`), `i_atime/ctime/mtime/dtime`, `i_gid` + `i_gid_high`, `i_links_count`, `i_blocks` (en **sectores de 512 B**), `i_flags`, `i_block[15]`, `i_generation`, `i_file_acl`. Si `s_inode_size > 128`, `i_extra_isize` y campos extra (nanosegundos, crtime): se **conservan** al reescribir el inodo y se ignoran.
- Mapa de bloques: 12 directos, 1 simple indirecto, 1 doble, 1 triple. Con bloque de 1 KiB: 12 + 256 + 65 536 + 16 777 216 bloques. Ficheros dispersos (bloque 0 = agujero) deben leerse como ceros y **no** consumir bloques al escribirse parcialmente.
- **Enlaces simbólicos:** *rápidos* si el destino mide <60 bytes **y** `i_blocks == 0` (el texto vive en `i_block[]`); *lentos* en un bloque de datos. Al crear: rápido si cabe. Criterio de detección tal como lo hace `e2fsck` (`i_blocks` menos bloques de EA), no solo por longitud.
- Directorios: lista lineal de `{inode:u32, rec_len:u16, name_len:u8, file_type:u8, name[]}` (con `filetype`; sin él `name_len` es u16). Registros alineados a 4, no cruzan límite de bloque, `inode == 0` = libre, `rec_len` encadena (el último llega al final del bloque). **`dir_index` (htree):** se lee **recorriendo linealmente** (los bloques internos del árbol parecen entradas vacías de `rec_len = bloque`, así que un recorrido lineal es correcto). **Al modificar** un directorio con `EXT2_INDEX_FL` se **borra el flag** y se trata como lineal (los bloques internos pasan a ser espacio libre válido). `e2fsck -fD` lo reindexa. Esto lo verificará el oráculo de §7.
- Inodos especiales: 1 = bloques malos, 2 = raíz, 7 = GDT reservada (resize), 11 = `lost+found` (en rev 1 `s_first_ino`). Inodos < `s_first_ino` nunca se asignan.

---

## 3. Arquitectura en AuroraOS

### 3.1 Cambios previos en el VFS (fase E0)

1. `vfs_stat_t` + `nlink`, `atime`, `ctime`, `blocks`, `blksize`; `struct stat/statx` los exponen (comprobar `k_stat/k_statx`); `vfs_node_t` + `nlink` (los FS que no lo saben devuelven 1).
2. `utimes` hoy solo cambia `mtime`: añadir `utimens(node, atime, mtime)` (`utimensat` ya existe como syscall).
3. ~~`rename` con sobrescritura~~: ya soportada por tmpfs y FAT32; solo faltaba corregir el comentario de `vfs.h`. Pendiente aquí: `RENAME_NOREPLACE`/`RENAME_EXCHANGE` (opcional).
4. **Nombres**: ext2 admite 255 bytes; `VFS_PATH_MAX` = 128 limita ruta completa y `dirent.name`. **Decisión D5-b (Alvaro): se amplía en E0** (ver E0.0): nombre de componente 255 bytes (`NAME_MAX`) y ruta completa mayor (objetivo 4096, a fijar tras medir el coste en `vfs_node_t`/`tmpfs_node`; `node->name` deja de guardar la ruta completa si hace falta).
5. **Opciones de montaje**: `struct vfs_mount` guarda flags (`MS_RDONLY`, `MS_NOSUID`, `MS_NODEV`, `MS_NOEXEC`, `MS_NOATIME`, `MS_SYNCHRONOUS`) y `execve`/`open` los consultan (`nosuid` pendiente de U1.4). Syscalls `mount(2)` (tipos `ext2`, `vfat`, `iso9660`, `tmpfs`, bind y `MS_REMOUNT`) y `umount2(2)`; `vfs_umount` falla con `-EBUSY` si hay nodos abiertos o mounts anidados.
6. `vfs_fs_ops_t` gana opcionales: `sync(priv)`, `remount(priv, flags)`, `umount(priv)`, `probe(bdev)`.

### 3.2 Caché de inodos por FS

`ext2_fs_t` guarda una tabla hash `ino → ext2_inode_t` con refcount (`vfs_node.priv` apunta al `ext2_inode_t`, no a una copia). Contiene el inodo en memoria (mode, uid, gid, size, tiempos, `i_block[]`, `links`, `i_blocks`), flag `dirty`, y un `rwlock` por inodo. Reglas:

- `lookup` por ruta: resuelve componente a componente con `ino` (nunca fía de `node->name`; una renombrada de directorio no debe romper nodos abiertos).
- **Inodo desenlazado pero abierto** (`links == 0`, refs > 0): sus bloques se liberan al soltar la última referencia (`close`), no antes. Lista de huérfanos en memoria desde E2 y en disco (`s_last_orphan`) en E4.
- Límite de entradas con evicción de las no referenciadas (p. ej. 1024).

### 3.3 Caché de bloques (nuevo `kernel/bcache.c`)

API propia, independiente del FS: `bc_get(bdev, blkno, bs) → buf` (leído o nuevo), `bc_read`, `bc_mark_dirty`, `bc_put`, `bc_sync_dev(bdev)`, `bc_sync_range`, `bc_invalidate_dev(bdev)`. Bloque lógico de 1–4 KiB sobre `sector_size` del dispositivo (traduce a `bdev_read/bdev_write` en múltiplos de sector; para CD de 2048 B solo lectura).

- LRU con hash `(bdev, blk)`, refcount de pins, bits `dirty/uptodate/locked/meta`.
- Tamaño por defecto: ≈ 1/16 de la RAM libre con tope de 64 MiB y mínimo 2 MiB (configurable en cmdline `bcache.mb=`).
- **Write-back** con un kthread `bcache-flush` que despierta cada ~5 s o al pasar un umbral de sucios; los metadatos se escriben **antes** que los datos de la misma tanda (§5).
- Lecturas anticipadas para ficheros secuenciales (E7). Lecturas/escrituras contiguas se agrupan en un solo `bdev_read/bdev_write` con `count` grande (el driver ya trocea por `AHCI_MAX_XFER_BYTES`).
- Alineación de buffers: reutilizar la lección del bug BMIDE (buffers alineados a ≥16 B y, mejor, a página).
- Barreras: `bc_barrier(bdev)` = escribir pendientes marcados "antes" + `bdev_flush`.
- Errores de E/S: el buffer queda `error`, el FS recibe `-EIO`, y según `s_errors` pasa a ro (§5.4).

### 3.4 Sincronización global

Registro de FS montados con caché (`vfs_register_syncable`). `sync(2)`/`syncfs(2)`/`fsync(2)`/`fdatasync(2)` dejan de ser no-ops para ext2; `power.c` llama a `vfs_sync_all()` (que incluye `fat32_sync_all` y `ext2`) antes de matar procesos y de nuevo después (ya hace eso con FAT32); `umount` desmonta limpio. Se corrige también el comentario engañoso de `k_sync`.

### 3.5 Módulos nuevos

| Fichero | Contenido |
|---|---|
| `kernel/ext2.h` | structs de disco (`__attribute__((packed))`, campos LE con helpers aunque x86 sea LE), constantes, API pública |
| `kernel/ext2.c` | mount/umount/statfs/lookup + `vfs_ops_t` |
| `kernel/ext2_balloc.c` | asignación y liberación de bloques e inodos |
| `kernel/ext2_inode.c` | caché de inodos, `bmap` (directos/indirectos), truncar |
| `kernel/ext2_dir.c` | iteración, búsqueda, inserción, borrado de entradas |
| `kernel/ext2_check.c` | comprobación ligera de solo lectura (para `ASYS_FS_CHECK`, §6.3) |
| `kernel/bcache.c/h` | caché de bloques (§3.3) |
| `kernel/fsprobe.c` | sondeo `bdev → tipo` por magia: FAT32 (BPB), ext2 (`0xEF53` en offset 1080), ISO9660 |
| `tests/ext2/` | generadores de imágenes del host y casos (§7) |

Se sigue el estilo de `fat32.c`: mutex por FS (el de FAT32 es propio porque el mutex general no servía en un contexto concreto; para ext2 usar el `mutex_t` normal salvo que aparezca el mismo problema), logs con prefijo `[EXT2]`, contadores visibles en `/proc/fs/ext2/<dev>` o `sysfs`.

---

## 4. Fases

Cada fase tiene un **hito verificable** en QEMU con una imagen creada por el host; ninguna se da por buena sin `e2fsck -fn` limpio cuando escribe.

### E0 — Cimientos (M–L) — **HECHA (2026-11-01)**: E0.0 a E0.4, ver commits de la rama `claude/project-thread-uq4oep`.
- E0.0 **Ampliar `VFS_PATH_MAX`/`vfs_dirent_t.name`** (D5-b): auditar todos los usos (`tmpfs_node`, `vfs_node_t`, `fat32_make_node`, `kmalloc` de nodos, `getdents`), separar `VFS_NAME_MAX=255` de `VFS_PATH_MAX`, regresión de `ls`, `find`, `rm -rf`, `git`. Va **primero y en su propio commit**.
- E0.1 Cambios de VFS de §3.1 (stat extendido, `utimens`, flags de montaje, `mount(2)`/`umount2(2)`, `fs->sync`). Compatibilidad: FAT32/tmpfs/tarfs/iso9660 siguen igual (los campos nuevos tienen valores por defecto).
- E0.2 `bcache` (§3.3) con prueba de kernel propia (lectura/escritura/evicción/flush sobre `tests/test_disk.img`).
- E0.3 `vfs_sync_all` (§3.4) y enganche en `power.c`.
- E0.4 `fsprobe` y reescritura de `mount_boot_media` para usar el sondeo (FAT32 e ISO9660 siguen funcionando igual; `try_mount_data` acepta ext2).
- **Hito E0:** `make iso` arranca igual que antes (`scripts/check_boot.sh`), `mount -t vfat` y `mount -t tmpfs` funcionan desde el shell, `rename` sobrescribe en tmpfs y FAT32, y los tests existentes de kernel no empeoran (los 5 `pmm` que ya fallan se dejan como están).

### E1 — Lectura (M) — **HECHA (2026-11-01)**: `kernel/ext2.c`; matriz de 8 imágenes del host (stat + sha256 coinciden), ext4 rechazado, 30 imágenes corruptas sin caída. Sin caché de inodos todavía (no hace falta en solo lectura; llega con E2). Pruebas: `tests/ext2/{gen_images,run_read_test,run_matrix,gen_fuzz,run_fuzz}`.
- E1.1 Superbloque, validación y features (§2.1–2.2); descriptores de grupo; `statfs`.
- E1.2 Caché de inodos, `bmap` completo (12/1/2/3 niveles), huecos, tamaño de 64 bits.
- E1.3 Directorios (lineales, `dir_index` por recorrido lineal), `lookup`, `readdir` (`d_type` desde `file_type` o `i_mode`), `readlink` (rápido y lento), `stat` completo con uid/gid de 32 bits, `nlink`, `blocks`.
- E1.4 `read` con lecturas contiguas agrupadas, `get_page` para `mmap(MAP_SHARED)` de solo lectura, ejecución de binarios desde ext2 (`execve` pasa por `read`).
- E1.5 Montaje forzado **solo lectura** (aunque se pida rw) mientras no exista E2; rechazo limpio de features no soportadas.
- E1.6 Endurecimiento: todo índice de disco se valida (número de inodo/bloque dentro de rango, `rec_len` ≥ 12, múltiplo de 4 y que no se salga del bloque, ciclos de directorios, símbolos de longitud absurda). Un disco corrupto **jamás** debe tirar el kernel.
- **Hito E1:** imágenes de la matriz de §7.1 (1 KiB/2 KiB/4 KiB, rev 0/1, 128/256 B) se montan en QEMU; `ls -laR` y `sha256sum` de todos los ficheros coinciden con el manifiesto generado en el host; ficheros con tind, dispersos, symlinks 59/60/61, directorio de 5000 entradas y nombres de 100 caracteres correctos; `mount -o ro` de una imagen ext3 limpia funciona; un ext4 se rechaza con mensaje.

### E2 — Escritura de datos y estructura (L)
- E2.1 Asignación de **bloques**: bitmaps en caché, objetivo `goal` (siguiente al último bloque del fichero; si no, el primer grupo con espacio empezando por el del inodo), reserva `s_r_blocks_count` para root, contadores por grupo y en superbloque (en RAM; se escriben en sync).
- E2.2 Asignación de **inodos**: directorios se reparten entre grupos (estilo Orlov simplificado: el grupo con más inodos libres y por encima de la media), ficheros en el grupo del padre; honrar `s_first_ino`.
- E2.3 `write` (crece, sobrescribe, agujeros, `O_APPEND`, escrituras parciales de bloque con *read-modify-write*), puesta a cero del resto de bloque nuevo, `truncate`/`ftruncate` (acortar libera indirectos vacíos; alargar crea hueco), `fallocate` modo 0 como relleno simple (opcional).
- E2.4 `create`, `mkdir` (`.`/`..`, `used_dirs`, `nlink` del padre +1), `unlink`, `rmdir` (solo vacío), `mknod` (FIFO/chr/blk/socket: `i_block[0]` = rdev), `symlink` (rápido/lento), tiempos (`rtc_get_epoch`).
- E2.5 Inserción en directorios: reutilizar hueco (`rec_len` sobrante) o añadir bloque; borrado fusionando con el registro previo; el directorio nunca se encoge salvo a 0 entradas en `rmdir`.
- E2.6 Estado del FS: al montar rw se escribe `s_state` sin `VALID_FS`, `s_mnt_count++`, `s_mtime`, `s_last_mounted`; flush + barrera. En `umount` limpio: volcar todo, poner `VALID_FS`, `s_wtime`, flush.
- **Hito E2:** en QEMU, sobre una copia de la imagen: crear 10 000 ficheros, escribir un fichero de 300 MB disperso, truncar, borrar, `mkdir -p` profundo; apagar con `poweroff`; en el host `e2fsck -fn` da **0** y el contenido coincide con el esperado (script de §7.3).

### E3 — Semántica POSIX completa (L)
> Estado: **E3 hecha** (rename, link, unlink con inodo abierto, chmod/chown/utimes/fsync, fchownat, O_SYNC y MS_SYNCHRONOUS, noexec y nodev; `tests/ext2/run_posix_cases.sh` compara 52 casos con Linux: 50 idénticos). Diferencias conocidas: `rename(".")` da EINVAL (Linux EBUSY) y un bucle de symlinks da ENOENT (Linux ELOOP), ambas del VFS. `MAP_SHARED` escribible **hecho después** (ver abajo). `nosuid` no tiene efecto porque execve no aplica setuid todavía.
- E3.1 `rename`: mismo dir y entre dirs, sobrescritura, directorio→directorio (actualiza `..` y `nlink` de los padres, **prohíbe mover un dir dentro de sí mismo**), orden de operaciones tolerante a cortes (§5.2).
- E3.2 `link` (hard links; `nlink` máx. 32000 en ext2, `-EMLINK`), `unlink` con inodo abierto (huérfano), contar `nlink` de directorios.
- E3.3 `chmod`, `chown` (uid/gid de 32 bits en `i_uid_high/i_gid_high`), `utimens`; limpiar setuid/setgid al escribir o cambiar propietario (como Linux), `S_ISVTX` y reglas de borrado/rename en dirs sticky (el VFS ya lo comprueba tras U1.2).
- E3.4 Permisos en el propio ext2: el propietario del fichero nuevo es `fsuid/fsgid` del proceso; `S_ISGID` en el directorio hace que el hijo herede el grupo (y los subdirectorios heredan el bit).
- E3.5 `fsync`/`fdatasync` reales (volcar inodo + bloques del fichero + barrera), `O_SYNC`/`MS_SYNCHRONOUS`.
- E3.6 `statfs` real (libres, disponibles para no-root descontando reservados, inodos), `MS_REMOUNT` `ro↔rw` (flush y estado), `nosuid/noexec/nodev` aplicados.
- E3.7 `mmap(MAP_SHARED)` escribible con `get_page` (marcar bloque sucio; asignar bloque en el primer fallo de escritura si hay hueco) — si resulta demasiado, queda `MAP_PRIVATE` y `MAP_SHARED|PROT_READ` y se documenta.
- E3.8 Suite POSIX compartida (la misma para tmpfs y ext2): permisos, sticky, setuid, rename, link, symlink, ciclos, ENOTEMPTY, ENAMETOOLONG, EXDEV, truncate, O_APPEND concurrente.
- **Hito E3:** el hito de U2 del plan de usuarios **sobre ext2**: `chown 1000:1000 /data/x; chmod 600 /data/x`, `poweroff`, arrancar, `stat` conserva uid/gid/modo; `git init/add/commit` y `python`/`perl` funcionan sobre ext2 como hoy sobre FAT32; `cred_test` pasa con `/data` en ext2.

> **mmap(MAP_SHARED) escribible (hecho):** `get_page` devuelve páginas de 4 KiB de una caché por inodo (solo de lo que alguien mapeó). La bcache sigue siendo la verdad de read()/write(): write() y truncate actualizan las páginas, y read(), msync, fsync, sync, umount, el cierre del último nodo y el hilo de volcado copian página→bcache cuando difieren (se compara; no se sabe si un proceso escribió). Un mapeo no alarga el fichero (más allá del final = SIGBUS). `PROT_WRITE` exige `O_RDWR` y montaje rw (EACCES si no). Límites: si falta espacio al volcar una página sobre un hueco se avisa en el log y el cambio se pierde (Linux daría SIGBUS al fallar); `MAP_SHARED` de solo lectura también pasa por esta caché. Prueba: `tests/ext2/run_mmap_test.sh` (18 pasos idénticos a Linux, 1K y 4K).

### E4 — Consistencia (M)
> Estado: **hecha en lo esencial.** Liberaciones diferidas (un bloque o inodo liberado solo vuelve a ser asignable tras una barrera que deje en disco el inodo/directorio que ya no lo referencia). `tests/ext2/run_crash_test.sh`: 20 cortes con `kill -9` de QEMU en mitad de una carga aleatoria: 19 correctos (`e2fsck -fy` repara y un segundo `-fn` da 0, sin bloques duplicados, ficheros no tocados intactos) y 1 sin resultado porque el invitado no llegó a montar. No se hicieron las 200 iteraciones del hito (cada una tarda minutos en TCG). **Cierre (2026-11-02):** hechos `s_last_orphan` (los inodos sin enlaces pero abiertos entran en la lista de huérfanos en disco y salen al liberarse; el último de la cadena lleva `i_dtime=0`, e2fsck lo trata como en ext3), `s_mnt_count`/`s_mtime` al montar rw y aviso si se supera `s_max_mnt_count`/`s_checkinterval`, `/lost+found` creado al montar rw si falta, y `ext2_check()` al montar (recuenta los bitmaps, corrige en RAM los contadores de grupo y totales y los persiste al pasar a rw). Prueba: `tests/ext2/run_e4rest_test.sh`. Sigue sin hacerse `data=ordered` (el diferido de liberaciones cubre lo peligroso) y quedan 20 de 200 iteraciones de corte de corriente.
Ver §5. Orden de escritura, huérfanos en disco (`s_last_orphan`), política `s_errors`, intervalo de `fsck` (`s_max_mnt_count`/`s_checkinterval`) y `lost+found` obligatorio.
- **Hito E4:** prueba de **corte de energía** (QEMU `kill -9` en medio de cargas aleatorias, 200 iteraciones): tras cada corte `e2fsck -fy` termina en ≤ 2 pasadas, nunca informa *bloques duplicados* ni pérdida de directorios enteros, y los ficheros que **no** se estaban escribiendo conservan su contenido (el oráculo compara con una copia).

### E5 — Montaje, arranque y `/data` (M)
- E5.1 `mount_boot_media`: ext2 en `/data` por etiqueta (`AURORADATA`) o `cmdline data=…`; si el FS no está limpio ⇒ montar **solo ro** y avisar (política D3); opción `fsck.repair` en cmdline para reparar (§6).
- E5.2 `Makefile`: `USB_DATA_FS=fat32|ext2` (por defecto fat32 hasta pasar E4); con ext2 usa `mke2fs -t ext2 -b 4096 -L AURORADATA -m 0 -O ^resize_inode -d <árbol-semilla>` (`-d` copia un árbol semilla y `-E root_owner=…` fija el propietario de la raíz) y cambia el tipo MBR de la partición 2 de `0x0c` a `0x83`.
- E5.3 `/etc/fstab` mínimo; `mount -a` en el arranque (util-linux `mount` precompilado lo usa con `mount(2)`); `/proc/mounts` coherente con `mountinfo`.
- E5.4 `klog.file=/data/aurora.log` y `last_kmsg.log` (`save_prev_log_to_data`) funcionan sobre ext2; el log en vivo usa `fsync` con la frecuencia actual, ahora respaldado por la caché (comprobar impacto: ~1 escritura/100 ms).
- **Hito E5:** ISO con `USB_DATA_FS=ext2`: arranca en QEMU y en el pendrive, `/data` es ext2 rw, el log en vivo se guarda, tras `poweroff` el host monta el USB y lee el log.
- **Estado E5 (2026-11-02): E5.1 a E5.4 hechas.** `USB_DATA_FS=ext2` (por defecto sigue fat32) crea la partición 2 con `mke2fs` y tipo MBR 0x83. `try_mount_data` monta ext2 con etiqueta `AURORADATA` en rw (si devuelve -EROFS avisa y monta ro); otros ext2 se montan ro. Verificado en QEMU con arranque UEFI solo-USB: `/data` rw, `klog.file=/data/aurora.log` en vivo, y tras `power off` el host ejecuta `e2fsck -fn` limpio y lee `aurora.log` y los ficheros escritos. Tras corte brusco hace falta `e2fsck` en el host (sin journal hasta E6). E5.3 hecha: `/etc/fstab` (semilla del tmpfs de /etc) y `init` lanza `mount -a` en segundo plano con reintentos (los discos aparecen después). Probado con un segundo disco ext2. Falta probar en el pendrive real.

### E6 — Herramientas de usuario (M–L)
Ver §6. `e2fsprogs` portado con glibc: `mke2fs`, `e2fsck`, `tune2fs`, `dumpe2fs`, `debugfs`, `resize2fs` (este último opcional), más `blkid` de util-linux (ya hay `libblkid.so.1`).
> **Estado E6 (2026-11-02): hecha.** `scripts/build-e2fsprogs.sh` compila e2fsprogs 1.47.0 estático con la glibc del host (sin hilos, sin sigcatcher, sin chequeo de versión de kernel de mke2fs) y deja `mke2fs`, `e2fsck`, `tune2fs`, `dumpe2fs` y `debugfs` (+ enlaces `mkfs.ext2`, `fsck.ext2`, `e2label`...) en `precompiled/usr/sbin`; `/etc/mke2fs.conf` y `/etc/fstab` son semillas del tmpfs de /etc. Pruebas: `tests/ext2/run_e2fsprogs_test.sh` (mke2fs sobre disco en blanco, mount, copia, umount, `e2fsck -fp` rc 0, y el host da `e2fsck -fn` 0 y contenido idéntico) y `tests/ext2/run_e2fsck_repair_test.sh` (e2fsck de AuroraOS repara un FS estropeado en el host: rc 1 y luego rc 0; el host lo ve limpio). Hallazgos en el kernel: `execve` no ponía a cero los registros (`%rdx` es el puntero de atexit para el `_start` de glibc estático: saltaba a basura al terminar cualquier binario estático); corregido. Pendiente de E6: `blkid`/`resize2fs`, e2fsck interactivo (hace falta tty) y el USB real.
- **Hito E6:** desde AuroraOS: `mke2fs -t ext2 /dev/sdb1` sobre un USB, montarlo, copiar, `umount`, `e2fsck -f` (exit 0), y el host monta ese mismo USB.

### E7 — Rendimiento y raíz instalada (L)
- E7.1 Caché de directorios/`lookup` (el VFS resuelve por ruta y recorre linealmente cada directorio: coste O(profundidad × entradas)); lectura de **htree** para directorios grandes (solo lectura, es el caso crítico de `node_modules`/`/usr/lib`); lecturas anticipadas; escritura agrupada; preasignación de bloques contiguos al crecer (p. ej. 8).
- E7.2 Un lock por inodo + uno por grupo (en vez de un mutex global) cuando haya cargas SMP reales.
- E7.3 **Instalador** (`aurora-install`, script + `fdisk`/`mke2fs`): particiona (GPT: ESP FAT32 con kernel+`aurora.conf`, raíz ext2), crea el FS, copia el sistema conservando uid/gid/modo/enlaces/dispositivos, escribe `aurora.conf` con `root=UUID=…`. **No hace falta lector de ext2 en el bootloader**: el kernel y el initrd viven en la ESP FAT32 y el kernel monta la raíz ext2 y hace `vfs_pivot_root` (ya existe). El bootloader solo aprende a pasar `root=` si no lo hace ya (verificar `bootloader/aurora.conf`).
- E7.4 `fsck` en arranque: si `s_state` no es limpio o se superó `max_mnt_count`, el init ejecuta `e2fsck -p` sobre la partición **antes** de montar rw (si `e2fsck` existe en la imagen) y reintenta.
- **Hito E7:** instalación en QEMU desde la ISO a un disco virtual, reinicio sin ISO, login y `/` en ext2 con `uid/gid` correctos; `ls` de un directorio de 5000 entradas < 50 ms.

### E8 — (Opcional, después) ext3 limpio y ext4 de lectura
Para montar discos Linux reales (los `mke2fs` de las distros crean ext4): lectura de **extents**, `flex_bg`, `64bit`, `huge_file`, `dir_nlink`, `extra_isize`, htree; rechazo de `metadata_csum` rw (ro verificando o ignorando checksums). Escritura ext4 y journal quedan fuera de este plan.

---

## 5. Consistencia sin journal

ext2 no tiene journal: la garantía es "tras un corte, `e2fsck` puede reparar sin perder lo que no se estaba tocando". Para eso el driver sigue un **orden de escritura** y usa barreras.

### 5.1 Principios
1. Un puntero nunca se escribe antes que lo apuntado. Los bitmaps y contadores son redundantes (e2fsck los recalcula): se retrasan.
2. Se **asigna antes de enlazar**, se **desenlaza antes de liberar**.
3. Ninguna operación deja un inodo o bloque *referenciado* y *libre* a la vez. Sí puede dejar bloques/inodos *ocupados y sin referencia* (fugas): es lo que `e2fsck` arregla gratis.

### 5.2 Orden por operación
| Operación | Orden de escritura |
|---|---|
| crear fichero | bitmap de inodos → inodo (nuevo, `links=1`) → entrada de directorio → inodo del padre (mtime) |
| mkdir | bitmap inodos + bloque → bloque del dir (`.`/`..`) → inodo nuevo → entrada en el padre → inodo del padre (`nlink+1`) |
| unlink | entrada de directorio → inodo (`links-1`, `dtime` si 0) → bitmap de inodo y bloques |
| crecer fichero | bitmap de bloques → datos → bloque indirecto → inodo (tamaño/punteros) |
| truncar | inodo (nuevo tamaño y punteros) → bitmap de bloques |
| rename | enlazar en destino primero → quitar origen después (nunca queda sin nombre); si un dir cambia de padre, actualizar `..` antes de quitar el origen |

Los datos de ficheros ("ordered") se escriben antes del inodo que los hace visibles (`data=ordered` informal), para que un corte no exponga basura de bloques reutilizados.

### 5.3 Flush
- `s_state`: se limpia `VALID_FS` al montar rw y se reescribe al desmontar limpio (§E2.6).
- Barrera (`bdev_flush`) tras escribir el estado "sucio" en el montaje, entre "metadatos de asignación" y "metadatos de directorio" en operaciones que cruzan, y tras `fsync`.
- Cada `N` segundos el kthread de flush vuelca por tandas respetando dependencias simples (marca `must_precede` entre buffers; nada de grafo general).

### 5.4 Errores
`s_errors`: **remount-ro** por defecto (D3). Ante `-EIO` o una inconsistencia detectada (bitmap con bit ya libre al liberar, número de bloque fuera de rango, `rec_len` corrupto) se registra, se pone `ERROR_FS` en `s_state` y se pasa a ro; el kernel no entra en pánico. Un USB retirado en caliente produce `EIO` en cascada y debe acabar igual (el `block_proxy` ya tiene vigilancia por plazos).

### 5.5 `fsck`
- **Política de arranque (D3):** FS sucio ⇒ montar ro y avisar; nunca reparar automáticamente en segundo plano con el FS montado rw.
- **`e2fsck` real** (portado, §6) como único reparador. No se escribe un reparador propio.
- **Chequeo ligero en el kernel** `ext2_check()` (solo lectura, análogo a `fat32_check`): coherencia de contadores de grupo vs. bitmaps, inodos con `links=0` y bloques asignados, recorrido de la raíz hasta N inodos. Se expone por `ASYS_FS_CHECK` (hoy exige `"fat32"`) para que `aurora-fsck` lo use en diagnóstico rápido y para los tests; **no repara**.

---

## 6. Herramientas de usuario

### 6.1 Port de e2fsprogs con glibc
- Fuente oficial `e2fsprogs` (1.47.x, igual que el oráculo del host) compilada con el toolchain glibc, siguiendo el patrón de `scripts/fetch-*.sh` (script `fetch-e2fsprogs.sh` + receta en el Makefile). Instalar en `/sbin` y `/usr/sbin`. Sin dependencias raras: `libext2fs`, `libcom_err`, `libe2p`, `libuuid`, `libblkid` (ya hay `libblkid.so.1` en `libs/glibc`; usar la interna de e2fsprogs si hay conflicto).
- Requisitos de kernel que hay que comprobar antes (cada uno una prueba de `tests/ext2/`): `open`/`pread64`/`pwrite64` sobre `/dev/<bdev>` con desplazamientos grandes; `BLKGETSIZE64`, `BLKSSZGET` (existen) y **`BLKFLSBUF`, `BLKROGET`, `BLKPBSZGET`/`BLKIOMIN` (faltan)**; `fsync` sobre el dispositivo de bloque = `bdev_flush`; `stat` de `/dev/sdXN` con `S_IFBLK` y `st_rdev` (hoy `VFS_CHARDEVICE` es el único bit de dispositivo; verificar cómo se reporta); `flock`; `/proc/mounts` (o `mountinfo`) para que `e2fsck` detecte que está montado; `getrandom`/`/dev/urandom` para UUID; `mke2fs.conf` en `/etc`.
- **Prohibir mke2fs/e2fsck sobre un dispositivo montado rw** (el kernel responde `-EBUSY` al `open(O_WRONLY)` de un bdev con montaje rw: "exclusión de montaje" que Linux hace con `bd_claim`).

### 6.2 Coherencia con la caché del kernel
Si el FS está montado, el escritor crudo y el driver comparten disco ⇒ corrupción. Reglas: (1) `open(O_RDWR)` en un bdev con montaje rw ⇒ `-EBUSY`; (2) abrir en solo lectura se permite pero lee **a través de la caché del kernel** (`bc_read`) para ver lo último; (3) tras `umount`, el kernel invalida la caché del dispositivo (`bc_invalidate_dev`) y el siguiente montaje relee todo; (4) `e2fsck` sobre un FS montado ro (modo `-n`) es válido; sobre montado rw, rechazado.

### 6.3 Qué se instala en la imagen
`mke2fs` (+ enlaces `mkfs.ext2`), `e2fsck` (`fsck.ext2`), `tune2fs`, `dumpe2fs`, `debugfs`, `resize2fs` (si se usa), `/etc/mke2fs.conf` con perfil Aurora (`blocksize = 4096`, `inode_size = 128`, `features = filetype,sparse_super,large_file`, sin `resize_inode` ni `dir_index` en la v1). Un `fsck` envoltorio para elegir por tipo.

---

## 7. Pruebas

### 7.1 Imágenes generadas en el host (`tests/ext2/gen_images.sh`)
Todas con un árbol semilla determinista (script que genera ficheros con contenido derivado de un contador + manifiesto `sha256` + `stat`), vía `mke2fs -d`. La matriz mínima:

| Imagen | Opciones de `mke2fs` |
|---|---|
| `r0_1k` | `-t ext2 -r 0 -b 1024` |
| `r1_1k_min` | `-b 1024 -O ^dir_index,^resize_inode,^ext_attr,^large_file,^sparse_super` |
| `r1_1k_def` | `-b 1024` (defaults del host) |
| `r1_2k` | `-b 2048` |
| `r1_4k` | `-b 4096 -I 128` |
| `r1_4k_256` | `-b 4096 -I 256` |
| `r1_bpg` | `-g 2048` (muchos grupos, pruebas de bordes de grupo y GDT multibloque) |
| `r1_full` | casi lleno / sin inodos (`-N` bajo) para `ENOSPC` |
| `ext3_clean` | `-t ext3` (debe montar ro) |
| `ext4_def` | `-t ext4` (debe rechazarse) |
| `bad_*` | imágenes corruptas del fuzzer (§7.4) |

Contenido del árbol: ficheros de 0, 1, 1023, 1024, 4095, 4097 bytes; uno que cruza el 12.º bloque, el primer indirecto y el doble indirecto; uno con ≥1 GiB aparente y agujeros (para triple indirecto con 1 KiB); symlinks de 58, 59, 60, 61 bytes; hard links; FIFO, chr, blk, socket; directorio de 5000 entradas; nombres de 1 y 255 bytes; setuid/setgid/sticky; uid/gid >65535.

### 7.2 Oráculo del host
- Tras cada prueba de QEMU, la imagen se extrae y se pasa por **`e2fsck -fn` (exit 0 obligatorio)**; `dumpe2fs -h` comprueba contadores, `debugfs -R 'rdump / dir'` (sin root) saca el árbol y se compara con la expectativa (`sha256`, modos, uid/gid, enlaces).
- Orientación inversa: imagen creada **por AuroraOS** (`mke2fs` portado) y verificada por el `e2fsck` del host, y viceversa.

### 7.3 QEMU / CI
- Extender `scripts/qemu-boot-test.sh` con `EXT2_IMG=… scripts/ext2-test.sh <caso>` que arranca con la imagen como segundo disco (AHCI) y como USB (`-device usb-storage`), ejecuta un programa de prueba (`user/apps/ext2_test`, estilo `cred_test`) y devuelve el código; luego aplica §7.2. Un fichero de casos por fase (`E1_read.sh`, `E2_write.sh`, `E3_posix.sh`, `E4_powercut.sh`).
- Pruebas de kernel unitarias (el kernel ya tiene su arnés con "pruebas" que imprime al arrancar): bmap, asignador, inserción/borrado de directorios con casos de borde (`rec_len` extremos), parsing de superbloque con valores límite.
- **Suite POSIX común** tmpfs vs ext2 (§E3.8): mismo binario, mismos resultados esperados.

### 7.4 Fuzz y estrés
- **Fuzz de imágenes:** script que muta bytes/campos del superbloque, GDT, bitmaps, inodos y directorios de las imágenes de §7.1 (1000 mutaciones con semilla fija) y el kernel debe montar/listar/leer sin panic ni bucle infinito (límite de tiempo en QEMU); si monta rw, cualquier operación devuelve error en lugar de corromper.
- **Estrés de operaciones aleatorias** (`ext2_stress`: semilla fija, create/write/truncate/rename/link/unlink/mkdir/rmdir/symlink con 4–8 hilos), reproducido en tmpfs como referencia; al final se compara árbol y `e2fsck`.
- **Corte de energía** (E4): QEMU con `-drive cache=none`, `kill -9` en un instante aleatorio de una carga aleatoria; 200 iteraciones; la suite registra qué tipo de reparaciones hizo `e2fsck`.
- **Hardware real:** el USB de pruebas y el portátil Toshiba (con la fuente al alcance, como Alvaro prefiere) solo tras pasar E4 en QEMU; la primera prueba en hardware es **solo lectura** de una partición ext2 de un pendrive, después rw con una copia.

---

## 8. Encaje con `Plan_Usuarios.md` (U2) y cómo lo sustituye

- **El problema de U2 es que FAT32 no guarda propietarios/modos**; DU1 propone un *sidecar* `/data/.aurora-meta` ahora y ext2 "después, en plan aparte" (U2.5). Con este plan, **ext2 pasa a ser la solución de U2.1** para `/data` y el *sidecar* deja de ser necesario donde `/data` sea ext2: los modos, uid y gid van en el inodo.
- Lo que **sigue haciéndose igual**: U2.3 (`/etc` persistente en `/data/etc` con `fichero.tmp` + `rename`) funciona idéntico sobre ext2 — de hecho **requiere** E0 (rename con sobrescritura); U2.4 (semilla de `/etc`); U2.2 (`fmask/dmask/uid=/gid=` para FAT32 de otros sistemas) se reutiliza para pendrives FAT32 de terceros y comparte con E0.5 la tabla de **opciones de montaje**, que incluye el `nosuid` que quedó pendiente en U1.4. **Quien llegue primero crea esa tabla**; el otro la usa.
- **Orden recomendado (D2):** E0 → E1 → E2 → E3 a la vez que se hace U2.3/U2.4. Si E3 no está lista cuando U3/U4 la necesiten, se recurre al *sidecar* mínimo (DU1-a) como puente; **no** se construye de antemano.
- Cuando `/data` sea ext2, `fat32.c` deja de ser el camino crítico del plan de usuarios y `fat32_ram_mode_*` (modos en RAM) queda solo para FAT32.
- Dependencias: U3 (adduser/passwd) y U4 (login) pueden empezar sobre FAT32 con root-only, pero `/data/home/<usuario>` privado y `shadow 0640 root:shadow` exigen E3 (o sidecar). U5 (permisos de `/dev`) no depende de ext2.

Diagrama:

```
U0 ─► U1 ─► U2.3/U2.4 ─► U3 ─► U4 ─► U5 ...
            ▲
E0 ─► E1 ─► E2 ─► E3 ─► E4 ─► E5 ─► E6 ─► E7 ─► (E8)
      └ lectura   └ U2.1 sobre ext2 ┘   └ instalador, raíz ext2
```

---

## 9. Riesgos

| Riesgo | Mitigación |
|---|---|
| **Corrupción de datos del usuario** al escribir | Ninguna escritura rw hasta E2 con oráculo del host; `/data` sigue FAT32 por defecto hasta pasar E4; primera prueba real con una **copia** de datos; `ro` ante cualquier inconsistencia. |
| **Caché del kernel vs. herramientas** en espacio de usuario | Exclusión de montaje y lectura a través de la caché (§6.2) antes de portar `e2fsprogs`. |
| **Sin journal**: tras corte hay que correr `e2fsck` y un pendrive quitado sin `umount` queda "sucio" | Montar ro si sucio (D3), `sync` periódico agresivo para `/data` en LiveUSB (`commit=5`), `e2fsck -p` en arranque cuando exista. Es el compromiso de ext2; ext3-journal queda explícitamente fuera. |
| **Locking**: el VFS por rutas + nodos nuevos por `lookup` hace fácil introducir carreras entre dos `open` del mismo inodo | Caché de inodos única + mutex por FS en E2/E3; refinar en E7 con prueba de estrés SMP. |
| **`rename` con sobrescritura y directorios** (ciclos, `..`) es la parte más delicada | E3.1 tiene suite propia y verificación con `e2fsck` tras cada caso; no se mezcla con otros cambios. |
| **Ampliar `VFS_PATH_MAX`** (D5-b) toca tmpfs, FAT32 y todos los nodos | E0.0 aislado en su commit, con regresión de FAT32/tmpfs antes de seguir; medir memoria por nodo. |
| **Memoria**: caché de bloques + de inodos en un kernel con `kmalloc` simple | Tope configurable, evicción, y no más de una tabla de inodos de 1024 entradas por FS al principio. |
| **Fragmentación y rendimiento** (asignador simple, USB lento) | Preasignación y agrupación de E/S en E7; medir con `dd`/`git` antes y después. |
| **Port de e2fsprogs** consume tiempo (autotools, syscalls menos comunes) | Hacerlo después de E3 (cuando el FS ya es estable) y con la lista de requisitos de §6.1 como lista de comprobación; plan B: `mke2fs` del host para crear imágenes y `ext2_check` del kernel para diagnóstico. |
| **Atime** produce escrituras en cada lectura (USB, desgaste, apagados sucios) | `noatime` por defecto (D6). |
| **Compatibilidad hacia atrás de los campos nuevos de `vfs_stat`** (`nlink` etc.) con herramientas ya instaladas | Valores por defecto (nlink=1) y prueba de regresión de `ls -l`, `find`, `rm -rf` (el bug de "Circular directory structure" por `st_ino` fue real en FAT32). |

---

## 10. Resumen de tallas

| Fase | Contenido | Talla | Se prueba con |
|---|---|---|---|
| E0 | VFS (stat, rename, mount(2)), bcache, sync global, fsprobe | M–L | arranque igual, `mount -t vfat/tmpfs`, rename |
| E1 | Lectura completa y robusta | M | matriz de imágenes del host (hash) |
| E2 | Escritura: bloques/inodos/dirs/truncate | L | `e2fsck -fn` = 0 tras cargas |
| E3 | POSIX completo: rename/link/permisos/fsync/mmap | L | U2 sobre ext2, `git`, `cred_test`, suite POSIX |
| E4 | Orden de escritura, huérfanos, errores, corte de energía | M | 200 cortes + `e2fsck` |
| E5 | Mount/boot/`/data` ext2/Makefile | M | ISO con `USB_DATA_FS=ext2` |
| E6 | e2fsprogs con glibc | M–L | mke2fs/e2fsck dentro de AuroraOS |
| E7 | Rendimiento, htree, instalador, raíz ext2 | L | instalación completa en QEMU |
| E8 | ext3 limpio / ext4 de lectura (opcional) | L | montar discos Linux reales |

**Primer paso propuesto:** E0.1 + E0.2 (VFS y bcache) con E1 de lectura en paralelo, sin tocar el arranque ni `/data`; decidir D1–D4 antes de E2.

---

## 11. Decisiones

**Tomadas por Alvaro (2026-11-01):** D1-a, D2-b, D3-a, D4-a, **D5-b** (distinta de mi recomendación: se amplía `VFS_PATH_MAX` en E0.0), D6-a, D7-a, D8-a, D9-a. La tabla conserva las opciones originales.

| # | Decisión | Opciones | Recomendación |
|---|---|---|---|
| **D1** | Alcance del formato | (a) ext2 puro rw (rev 0/1), rechazar ext3/ext4; (b) además ext3 limpio y ext4 solo lectura (E8); (c) ext4 rw | **(a) ahora, (b) como E8 opcional.** ext4 rw implica extents, `flex_bg`, journal y checksums: es otro proyecto. |
| **D2** | `/data` y el *sidecar* de U2/DU1 | (a) seguir DU1: sidecar ahora, ext2 después; (b) saltarse el sidecar y esperar a ext2 E3; (c) sidecar y ext2 en paralelo | **(b)**, con el sidecar mínimo solo como puente si U3/U4 se adelantan a E3. El sidecar con renombrados y tabla `cluster→meta` tiene coste comparable al camino básico de escritura de ext2 y se tiraría. |
| **D3** | Política de consistencia | (a) ext2 sin journal, montar **ro** si sucio, `e2fsck` manual/en arranque; (b) reparar siempre automáticamente al arrancar; (c) implementar journal (ext3/jbd) | **(a).** Sin journal es lo que "ext2" significa; (c) es otro proyecto. `s_errors = remount-ro`. |
| **D4** | Herramientas fsck/mkfs | (a) portar `e2fsprogs` con glibc; (b) escribir `mke2fs`/`e2fsck` propios | **(a)**, tras E3. Un `e2fsck` propio sería enorme y menos fiable que el original; el kernel solo trae el chequeo ligero de §5.5. |
| **D5** | Nombres y rutas largas | (a) v1: `-ENAMETOOLONG` >127 bytes, ampliar `VFS_PATH_MAX` aparte; (b) ampliar ya `VFS_PATH_MAX` a 255/4096 como parte de E0 | **(a).** Ampliar toca `tmpfs`, `vfs_node_t` y todos los `kmalloc` de nodos; no mezclarlo con el driver. |
| **D6** | `atime` | (a) `noatime` por defecto; (b) `relatime`; (c) `strictatime` | **(a)**, con `-o relatime` disponible. |
| **D7** | Bloque al formatear (`/data` del LiveUSB de 64 MB; discos grandes) | (a) 4096 siempre; (b) 1024 para <512 MB como `mke2fs` por defecto | **(a) 4096** (menos metadatos y E/S, igual al tamaño de página). El driver **lee** 1024/2048/4096 de todas formas. |
| **D8** | Raíz instalada | (a) kernel+initrd en ESP FAT32, raíz ext2 con `pivot_root` (sin lector ext2 en el bootloader); (b) bootloader capaz de leer ext2 y arrancar de ahí | **(a).** No hay que tocar `bootloader/` salvo pasar `root=`; (b) queda para cuando se quiera `/boot` en ext2. |
| **D9** | Cuándo hacer ext2 el `/data` por defecto en la ISO | (a) tras E4 (corte de energía limpio); (b) tras E3; (c) nunca, opción `USB_DATA_FS` | **(a).** Hasta entonces `USB_DATA_FS=ext2` es opt-in. |

## Estado E7 (2026-11-04)

- **Caché de bloques con retención** (`kernel/bcache.c`): hash de 65536 cubos,
  `bc_get_cached`, `bc_readahead` (hasta 64 bloques), volcado coalescido de
  bloques sucios contiguos (hasta 64 bloques por escritura), `bc_autosize()` y
  `bc_reclaim()` desde kswapd cuando la RAM libre baja de la marca alta. Un
  fichero releído (cc1, as, ld) se sirve de memoria: 12,5 MB/s en caliente
  frente a 2,3 MB/s en frío en QEMU.
- **E7.1** caché de nombres (dir, nombre) -> inodo para directorios de >= 2
  bloques (32768 entradas, se rellena en lookup y readdir, se invalida al
  borrar/renombrar/liberar el dir): `ls -l` de 10000 ficheros pasa de >50 s a
  11 s en QEMU. Sin htree ni preasignación contigua (no hacía falta).
- **E7.4** `aurora-fsck` (init): si AURORADATA estaba sucia y el kernel la montó
  ro, ejecuta `e2fsck -p` y remonta rw si el código es <= 1. Se desactiva con
  `fsck.repair=0`; no corre en modo boot-tests. Test: `run_autofsck_test.sh`.
- **E7.3 Instalador: aparcado** por orden de Alvaro (hilo aparte).
- init: `waitpid` de las líneas de boot-tests reintenta ante EINTR (antes un
  SIGCHLD solapaba comandos y falseaba tests).

## Estado E8 y montaje al arrancar (2026-11-05)

- **E8 (ext4 solo lectura) hecha:** extents (árbol de cualquier profundidad;
  los extents sin inicializar leen ceros), `64bit` (descriptores de 64 B, FS
  < 2^32 bloques), `flex_bg`, `huge_file`, `dir_nlink`, `extra_isize`,
  `metadata_csum` (no se verifica), htree (se lee en lineal) y journal sin
  recuperar (se lee tal cual, con aviso). Siguen rechazados `meta_bg`, `mmp`,
  `inline_data`, `encrypt`, `casefold`, `ea_inode` y bloques > 4096. Cualquier
  FS con features ext4 se monta siempre `ro` y no ejecuta `ext2_check`.
  Test: `tests/ext2/run_ext4_test.sh` (64bit+csum+flex_bg y la variante sin
  ellas; 5 MB, fichero con 300 extents, disperso, htree de 3000 entradas,
  symlinks rápido y lento): sha256 y tamaños idénticos al host.
- **Montaje al arrancar:** solo se monta lo que cuelga de USB (`block_device_t.is_usb`,
  heredado por las particiones) y los volúmenes con etiqueta `AURORADATA`. Los
  demás discos conservan su nombre `/dev/sdX` (visible con `lsblk`/`blkid`) y se
  anota en el log que no se montan. `automount=all` en la línea del kernel
  devuelve el comportamiento anterior.

## Cierre de pendientes (2026-11-05)

- **lsattr/chattr:** ioctls `FS_IOC_GETFLAGS/SETFLAGS/GETVERSION` en ext2. Los
  flags `immutable` y `append-only` (solo root puede ponerlos) se imponen en
  write, truncate, unlink, rename, link, chmod, chown y utimes.
- **nosuid:** `mount -o nosuid` (y remount) hace que execve ignore setuid/setgid
  en ese montaje, para cualquier FS (antes solo FAT32).
- **mknod FIFO/socket por syscall:** crea el nodo en ext2 y tmpfs (tar, cp -a).
  Abrir un FIFO con nombre como tubería sigue sin existir en el VFS; los
  dispositivos de bloque por mknod siguen en EPERM.
- **data=ordered:** `bc_mark_dirty_data`: en cada volcado salen primero los
  bloques de datos de ficheros, se hace `bdev_flush` y después los metadatos.
  Tras el cambio los cortes de luz dejan el FS sin huérfanos pendientes en las
  14 iteraciones probadas (1 KiB con la carga aleatoria; 4 KiB con extracción
  de tar y carga paralela), todas con `e2fsck -fy` = 0.
- Tests: `run_pending_test.sh`, `run_crash_test.sh` con `SIZE/BS/LOADFILE/WDIR`.
- **Sigue fuera:** instalador E7.3 (a petición de Alvaro), abrir FIFOs con
  nombre, mknod de bloque, ioctls de ext4 no relacionados con flags.

## E9: escritura ext4 y journal (2026-11-07, rama claude/project-thread-uq4oep, sin fusionar a main)

- **E9.1 Journal jbd2:** al montar rw con `needs_recovery` se reproduce el journal interno (pasadas SCAN/REVOKE/REPLAY, csum v2/v3, revocaciones), se deja `start=0` y se quita `needs_recovery`. Journal externo y fast commit: se rechazan. Test: `tests/ext2/run_journal_test.sh` (oráculo `debugfs jr`).
- **E9.2/E9.3 Extents:** crear, ampliar, partir hojas (profundidad 0 y 1) y truncar. 64bit (descriptores de 64 B), flex_bg, huge_file, dir_nlink, extra_isize. Los ficheros y directorios nuevos usan mapa de bloques clásico (ext4 lo admite). Limitaciones: escribir en un extent sin inicializar (fallocate) da error; árboles de profundidad >1 no se amplían.
- **E9.4 metadata_csum:** crc32c de inodos, descriptores de grupo, bitmaps, cola de bloques de extents, cola de bloques de directorio, bloque xattr (refcount) y superbloque. Los grupos INODE_UNINIT/BLOCK_UNINIT se materializan al montar rw; `bg_itable_unused` se mantiene. Un directorio htree se desindexa (nodos vacíos con cola) en su primera modificación.
- **L1-L3 (cierre de límites):** extents genéricos (profundidad hasta 5, división de nodos con propagación al padre), escritura en extents sin inicializar (se parte el extent y solo ese bloque pasa a inicializado, a ceros) y ficheros/directorios/symlinks lentos nuevos con árbol de extents.
- **L4 Journal en escritura:** cada volcado de metadatos (bcache) es una transacción jbd2 completa (descriptores + bloques + commit, csum v3 si el journal lo lleva) que se checkpointea enseguida y deja el journal vacío. Los datos de ficheros siguen data=ordered (antes, con barrera). Con el journal activo el superbloque lleva needs_recovery mientras está sucio y un FS con journal que quedó sin desmontar se vuelve a montar rw tras reproducir el journal. Hook en `bcache.c` (`bc_set_journal`): los metadatos de un dispositivo con journal nunca salen de la caché sin pasar por él. Transacciones de hasta ~1000 bloques; el kernel sincroniza antes de pasar de 768 sucios. Soportado: journal interno con revoke/64bit/csum v3. No soportado (se escribe como antes, y un FS sucio queda en solo lectura): journal externo, csum v2, async/fast commit.
- **Cortes de luz en ext4:** `FSTYPE=ext4 MKOPTS=... [JCSUM=1] tests/ext2/run_crash_test.sh`. 8 iteraciones sin csum y 6 con csum + journal csum v3: reproducir solo el journal (`debugfs jr`) deja el FS consistente (`e2fsck -fn` = 0) en todas.
- **Pendiente:** profundidad máxima de extents 5; no se prueban los cortes de luz con cientos de iteraciones; sin probar en el USB real ni en el Portégé.
- **Test:** `tests/ext2/run_ext4w_test.sh` (FEAT=, BS=): e2fsck limpio con 1K/2K/4K, con y sin metadata_csum/64bit/flex_bg.
