# Task List — ELF64 User-Space Loader

- [x] Plan aprobado
- [/] Implementación

## Kernel — Modificaciones
- [x] `kernel/sched.h` — añadir campo `cr3` a `task_t` (offset 0x20)
- [x] `kernel/switch.asm` — guardar/restaurar CR3 en `task_switch`
- [x] `kernel/paging.h` — declarar 3 nuevas funciones
- [x] `kernel/paging.c` — implementar `paging_clone_kernel_space`, `paging_map_page_in`, `paging_free_user_space`
- [x] `kernel/sched.c` — actualizar `sched_init`, `sched_create_task`, `sched_create_user_task(fn, cr3)`
- [x] `kernel/elf.h` — structs ELF64 y API
- [x] `kernel/elf.c` — loader ELF64 con acceso físico directo
- [x] `kernel/process.h` — struct `process_t` y API
- [x] `kernel/process.c` — `process_spawn`, `process_exec`
- [x] `kernel/Makefile` — añadir `elf.o`, `process.o`

## Userspace — Nuevos ficheros
- [x] `user/syscall.h` — macros de syscall para Ring 3
- [x] `user/crt0.asm` — runtime de inicio (_start → main → SYS_EXIT)
- [x] `user/link.ld` — linker script (base 0x400000)
- [x] `user/apps/hello/main.c` — primer programa de demo
- [x] `user/Makefile` — compilar apps y copiar a sysroot

## Build System
- [x] `Makefile` (raíz) — añadir paso `user` antes de sysroot
- [x] `kernel/main.c` — reemplazar `create_user_demo()` con `process_exec("apps/hello")`

## Verificación
- [x] Build exitoso sin errores
- [x] Salida correcta en serial
