# Evaluación: ¿subir el reloj del kernel (KERNEL_HZ = 1000)?

Fecha: 2026-11-01. Sobre `main` (086b118b). Solo lectura de código; **sin mediciones en hardware** (el contenedor no tiene KVM). Los números son estimaciones y están marcados como tales.

## Conclusión

**No merece la pena subir a 2 kHz o más.** El coste se duplica y no arregla ninguno de los problemas reales, que no vienen de la frecuencia sino de que casi todo el tiempo está cuantizado al tick de 1 ms. Hay tres arreglos baratos que sí mejoran latencia y CPU, y un cambio medio (tickless en idle) que baja el coste actual en vez de subirlo.

## Cómo funciona hoy (kernel/time.c, sched.c)

- LAPIC timer en TSC-deadline (`lapic_timer_arm_tsc`), **periódico a 1 kHz en todos los CPUs**, también en idle (`idle_loop` hace `sti; hlt` y lo despierta el tick). No hay tickless.
- Cada tick en CPU0: `tick_count++`, `vdso_update_clock`, `compositor_frame_tick`, `tcp_timer_tick` (recorre 64 buckets con `spin_lock_irqsave` cada uno, aunque no haya conexiones), `ip_frag_timer_tick`, `ahci_poll_ports` (sale pronto si hay MSI) y `sched_tick`. `sched_wake_expired` va a 125 Hz o al vencer el plazo más cercano.
- `sched_tick` en todos los CPUs: contadores de carga, RCU quiescent, `process_account_tick`, y replanifica cada `SCHED_INTERVAL` = 10 ticks.
- Todo el tiempo del kernel es `tick_count` en ms: `clock_gettime` (incluida MONOTONIC), el vDSO (`mono_ns = tick_count * 1e6`), `nanosleep`, `poll`/`epoll` timeouts, rusage, futex, TCP RTO, ARP.

## Coste actual (estimado)

- Interrupciones: 1000/s por CPU. En el i7-12700KF (20 hilos) son ~20 000 IRQ/s con el sistema en reposo; en 4 CPUs de QEMU, 4 000/s.
- Cada tick cuesta ~1,5–3 µs (entrada/salida de IRQ, EOI, wrmsr del deadline, `sched_tick`; más en VM). Total en reposo: ~40 ms/s repartidos en 20 CPUs, es decir ~0,2 % de CPU agregada. **El coste en CPU es despreciable.**
- CPU0 añade ~1 µs/tick por los 64 lock/unlock de `tcp_timer_tick` (~0,1 %).
- El coste real es **energía y residencia en C-states**: despertar cada CPU 1000 veces/s impide estados profundos (C6 tarda >100 µs en salir). Importa en el portátil Toshiba, no en el escritorio.
- Subir a 2 kHz duplica todo esto (≈0,4 % agregado, el doble de despertares) sin ganar nada de lo de abajo.

## Qué falla de verdad (hallazgos)

1. **`usleep()` menor de 1 ms no duerme.** `k_nanosleep` convierte con `ts_to_ticks` = `ns / 1_000_000`, así que `usleep(500)` y `usleep(100)` dan 0 ticks, `deadline = now` y el bucle sale al instante. Es una espera activa. `xhci.aur` usa `usleep(500)` en `delay_ms` y en su bucle principal sin MSI, y `usleep(100)` en las esperas de comandos (xhci.c:472, 832, 1119, 2031): ahí está buena parte de la CPU que quema xhci.aur. Los de 1000 µs sí duermen, pero hasta el siguiente tick (0–1 ms de más). Con 2 kHz seguirían siendo 0 los de 100 µs.
2. **Los sleeps se truncan, no se redondean hacia arriba**, y se cuentan desde un instante a mitad de tick: `usleep(1500)` puede despertar tras ~1 ms.
3. **Reloj de 1 ms de resolución** también para MONOTONIC (`clock_getres` devuelve 1 ms). Afecta a timestamps de eventos de ratón, `time`, medidas de frames y cualquier código que mida latencias.
4. **El ratón no depende del tick.** Un ratón USB a 1000 Hz se resuelve por IRQ (MSI) del xHCI; el compositor limita a ~62 fps con `compositor_frame_tick` (cada 16 ticks). Con MSI roto o sin él, el polling de `xhci.aur` sí está atado a (1) y (2). Eso casa con "ratón lento y CPU alta".
5. `compositor_frame_tick` da 62,5 fps fijos, no 60 ni la tasa del monitor; para eso mejor vblank/pageflip (ya hay vblank por hardware en NVIDIA) que subir el tick.

## Alternativas, de más a menos rentable

| # | Cambio | Esfuerzo | Efecto |
|---|--------|----------|--------|
| A | `nanosleep`/`usleep`: redondear **hacia arriba** a 1 tick mínimo (no devolver 0 si ns > 0). Evita las esperas activas de xhci.aur y de cualquier driver | muy bajo (1 línea en `ts_to_ticks`) | Baja CPU de xhci.aur de golpe |
| B | `xhci.aur`: con MSI, bloquear en `epoll_wait` (ya está); sin MSI, `usleep(1000)` o epoll con timeout, y quitar `usleep(100/500)` salvo en el camino de recuperación | bajo (rama `claude/fix-xhci-mouse` ya mueve en esta dirección) | CPU y latencia del ratón |
| C | Reloj de ns: `clock_gettime`/vDSO desde TSC (`tsc_freq` ya calibrado; `mono = tsc_base + (rdtsc-tsc0)*1e9/freq`), con seqlock en vvar como ahora | medio-bajo | Resolución de ns gratis, sin tocar la frecuencia del tick |
| D | `tcp_timer_tick` solo si hay conexiones (contador global) o a 100–200 Hz; `process_account_tick` y contadores de carga igual | bajo | Menos trabajo por tick en CPU0 |
| E | **Tickless en idle** (NO_HZ_IDLE): en `idle_loop`, armar TSC-deadline a `min(g_next_wake_deadline, próximo periodo de compositor)` en vez del tick fijo, y recuperar `tick_count` desde el TSC al despertar | medio-alto (hay que reconstruir `tick_count` y cuidar cpu0 por vdso/TCP/ahci) | De ~20 000 a unas decenas de IRQ/s en reposo; mejores C-states |
| F | Timers de alta resolución (hrtimer sobre TSC-deadline one-shot) para sleeps <1 ms | alto | Solo si algún driver lo necesita de verdad; con A y B no hace falta de momento |
| G | Subir `KERNEL_HZ` a 2000–4000 | trivial en `time.h`, pero hay `/1000` y `1000ULL` codificados en `syscall.c` (ts_to_ticks, rusage, poll), `vdso.c` (`tick_count * 1000000`), `evdev.c`, `futex.c`, `arp.h` y `linuxkpi/jiffies.h` | **No recomendado** |

Subir KERNEL_HZ además rompe suposiciones (HZ = 1000 en LinuxKPI, `ms == tick`, `tick_count & 1023`, `& 15`, `% 10`) y obliga a revisarlas todas.

## Recomendación

1. Ahora: **A + B** (con lo que ya hay en `claude/fix-xhci-mouse`). Es lo que baja la CPU de xhci.aur con el dock arrancado.
2. Después: **C** (reloj de ns) y **D**.
3. Cuando se toque energía/portátil: **E** (tickless en idle).
4. No tocar KERNEL_HZ. Si más adelante algo necesita <1 ms, hacer F, no subir el tick global.

## Qué mediría para confirmarlo (en QEMU/KVM o en la máquina real)

- IRQ/s de timer y CPU de xhci.aur: `/proc/interrupts`-equivalente o el monitor de rendimiento, con y sin dock, antes/después de A+B.
- `perf`-like con TSC: coste de `time_tick` (rdtsc al entrar/salir en CPU0) para validar el ~1–3 µs estimado.
- Latencia de `epoll_wait(…,1)` y `usleep(1500)` con TSC antes/después de A.
