# Avisos del log de arranque (QEMU, scripts/qemu-drive.py -m 6G)

Revision de los WARN/ERROR del arranque y que se hizo con cada uno.

## Corregidos

| Aviso | Causa raiz | Arreglo |
|---|---|---|
| `[PF] uaccess fixup` (cientos por xkbcomp lanzado por Xwayland) | `execve` no borraba `rseq_ptr` de la tarea; `rseq_exit_to_user` escribia `cpu_id` / leia `rseq_cs` en la direccion de la imagen anterior (sin VMA y luego dentro del `.text` de la libc nueva) | `kernel/syscall.c`: execve pone a cero el estado rseq. El log del fixup incluye ahora `err` |
| `[AML] parse_data_object: opcode 0x.. desconocido` (x5) | `DefOpRegion` (`5B 80`) no lleva PkgLength, pero `parse_term_list` lo decodificaba y desincronizaba el cuerpo | `kernel/acpi_aml.c`: se quita el PkgLength. Namespace 291 -> 329 nodos |
| `Could not resolve keysym XF86...` (17, de xkbcomp) | `xkeyboard-config` mas nuevo que el `xkbcomp` copiado | `scripts/fetch-xwayland.sh` compila un keymap con el xkbcomp copiado y borra de `symbols/inet` las teclas cuyo keysym no resuelve; `precompiled/.../symbols/inet` ya regenerado |

## No son un fallo del kernel

- `[MPROTECT-SLOW]`, `[PF-SLOW]`: un memset de 4 KiB llega a 30 ms. Es QEMU sin KVM (TCG): primer toque de RAM del huesped y espera del shootdown de TLB a la otra vCPU. Son diagnosticos; en hardware o con KVM no deberian salir.
- `[WAIT] timeout sin efecto`: diagnostico informativo (plazo vencido que ya no despierta a nadie).
- `ERROR [ELF] overflow`, `kt_drv ya registrado`, `IRQF_ONESHOT`, etc.: pruebas negativas esperadas de la suite.
- `SKIP ... sin disco`, `SMEP/SMAP`, `TSC-deadline`, `EIST`, `_PSS/_PCT`: dependen de la configuracion de QEMU.

## Sin reproducir

- "double free": no aparece en ningun arranque (no sale `[SLAB] kfree con objeto ya libre`). Pendiente de un log de hardware real.

Verificacion: suite del kernel 201 pasan, 0 fallan, 16 omitidas; 0 avisos `uaccess` ni `Could not resolve`.
