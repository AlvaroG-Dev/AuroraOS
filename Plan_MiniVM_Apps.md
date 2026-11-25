# Plan: apps de Windows en AuroraOS con la mini-VM (aurora-vm)

Objetivo: abrir una app de Windows desde el Dock o desde Aplicaciones en pocos
segundos, sin ver el escritorio de Windows: solo la ventana de la app, con la barra
de título, los botones y la sombra de Aurora.

No hace falta ningún driver para Windows (Windows exige drivers firmados). Todo va
con lo que Windows ya trae: un puerto serie 16550 y un programa de usuario.

## Fase A: suspender y reanudar (arranque en segundos)

Windows arranca una vez. Al cerrar la última app, aurora-vm guarda el estado
entero en un fichero (`/data/win.snap`, junto al disco). La siguiente vez
reanuda desde ahí, como un portátil que hiberna. El disco queda coherente porque
cada sesión continúa la anterior; si no hay snapshot, se arranca en frío.

El fichero vive en el disco, así que sobrevive a apagar el PC. Caso típico:
enciendes AuroraOS por la mañana, abres una app de Windows y aurora-vm reanuda
el Windows que se quedó suspendido ayer, sin volver a arrancarlo. Solo el primer
arranque de todos (tras instalar) es en frío. Opcional: al iniciar sesión en
Aurora, `aurora-vm --precargar` lee el snapshot en segundo plano para que la
primera app abra todavía antes.

Si Windows necesita reiniciarse de verdad (actualizaciones), se hace en segundo
plano y se vuelve a suspender al terminar, sin que el usuario lo espere.

### Qué se guarda

| Parte | Cómo |
|---|---|
| RAM (memfd) | páginas no nulas, por bloques; las nulas son huecos |
| CPU | `AVM_GET_REGS` + nuevo `AVM_GET_CPU_EXTRA`: MSR del huésped (STAR, LSTAR, CSTAR, SFMASK, KERNEL_GS_BASE, TSC_AUX), FPU/SSE (fx+512), TPR, interrumpibilidad, SYSENTER |
| Dispositivos | PIC, PIT, RTC/CMOS, i8042, LAPIC/IOAPIC, IDE (con io_buffer), PCI/PM/port92, VGA (registros + VRAM) |
| Reloj | instante del huésped al suspender (ns y TSC) |

Solo se suspende en un punto limpio: sin interrupción a medio entregar (`reinj`),
sin IN pendiente y sin DMA en curso. Si no lo es, se deja correr unos ms y se reintenta.

Los dispositivos se guardan como bloques binarios con la versión del formato y un
identificador de compilación de aurora-vm. Si no coincide, se descarta el snapshot
y se arranca en frío, nunca a medias. Los punteros (`ide_if.bus`) y los fd se rehacen al cargar.

### El reloj al reanudar

- **now_ns:** el VMM pasa a usar `now_ns() = reloj del host - desfase`. Al
  reanudar se fija el desfase para que el tiempo del huésped siga donde quedó;
  así todas las fechas absolutas de los dispositivos (PIT, RTC, temporizador del
  LAPIC, temporizador PM) siguen valiendo sin tocarlas.
- **TSC:** con el control "TSC offsetting" y el campo VMCS TSC_OFFSET (0x2010),
  el TSC del huésped también continúa (tras reiniciar el host, el TSC real vuelve a 0).
- **Hora de Windows:** al despertar se pone al día con el RTC, como tras hibernar
  (el CMOS da la hora local real).
- **Atajos del kernel:** el atajo del RTC (`avm_fast.rtc_*_tsc`) convierte con
  el desfase para comparar con el TSC del host.

### Carga

- **Primera versión:** se lee el fichero entero al memfd, saltando los huecos.
  Unos segundos con un disco normal.
- **Después:** carga perezosa. El kernel mapea en el EPT páginas del fichero bajo
  demanda (COW sobre el memfd), y la app abre antes de leer los 3 GB.

## Fase B: agente en Windows y canal serie

- `aurora-agent.exe` (C, compilado con mingw, sin dependencias) se instala una vez:
  desde un CD de utilidades que genera aurora-vm (`--cd-utilidades`), se registra
  en `HKLM\...\Run` y arranca con la sesión.
- **Canal:** COM2 (16550 emulado en 0x2F8, IRQ 3), protocolo de líneas de texto
  con longitud. El VMM ya emula E/S de puertos; falta el 16550 con FIFO.
- **Órdenes:** `RUN <ruta> <args>`, `CLOSE <hwnd>`, `FOCUS <hwnd>`, `SIZE <hwnd> w h`, `KEY`/`MOUSE` (ver C).
- **Eventos del agente:** ventana nueva o cerrada (título, icono, clase, rectángulo,
  proceso), cambio de título o tamaño, proceso terminado.
- **Escritorio:** el agente oculta la barra de tareas y pone el fondo negro.

## Fase C: ventanas sueltas con estética Aurora

- **Pantalla:** la de Windows ya está en la memoria del VMM (LFB). Se usa un
  escritorio virtual grande, por ejemplo 3840x2160 o más, y el agente coloca cada
  ventana de nivel superior en su zona para que no se tapen entre sí.
- **Recorte y marco:** por cada ventana, aurora-vm crea una ventana de Aurora
  (Wayland, barra de 38 px de theme.h) y copia en ella el rectángulo cliente.
  El marco de Windows se quita: el agente elimina `WS_CAPTION`/`WS_THICKFRAME`.
  Los botones de Aurora mandan `CLOSE`, minimizar y maximizar.
- **Entrada:** se manda con coordenadas absolutas por el canal; el agente usa
  `SendInput` con `MOUSEEVENTF_ABSOLUTE` y el teclado como scancodes. El ratón
  PS/2 relativo queda solo para el modo escritorio completo.
- **Repintado:** solo se copia lo que cambió. Se compara por bloques o el agente
  avisa con rectángulos sucios.

## Fase D: integración

- **Lanzador:** aurora-run ya tiene el hueco del tipo "vm"
  (`/usr/libexec/aurora/aurora-vm`). Un `.desktop` con
  `Exec=aurora-vm --app "C:\\Programas\\app.exe"` sale en Aplicaciones y en el Dock.
- **Iconos:** el agente extrae el icono del .exe (ExtractIconEx) y aurora-vm lo
  guarda en `~/.local/share/icons`.
- **Portapapeles:** texto en los dos sentidos por el canal.
- **Ficheros:** carpeta compartida con `/home` (más adelante: SMB sobre la red
  virtual, o un disco FAT de intercambio).
- **Rendimiento:** en hardware real sin QEMU por debajo, las salidas de VM
  cuestan ~1 µs en vez de ~150-600 µs, y Windows va mucho más fluido.

## Disco y datos

- **Disco de Windows:** `C:` es un fichero de imagen en bruto en la partición de
  datos de AuroraOS (`/data/win.img`, creado con `aurora-vm --crear-disco`). Es
  disperso: solo ocupa lo que Windows ha escrito. Dentro va NTFS, con Windows,
  los programas instalados y la carpeta del usuario.
- **Suspensión:** `/data/win.snap`, al lado del disco, es la RAM y el estado de la
  CPU. Disco y snapshot van siempre juntos: copiar los dos con Windows suspendido
  es una copia de seguridad completa. Si se borra el snapshot, Windows arranca en
  frío con el disco tal cual, como tras un corte de luz.
- **Un solo uso:** aurora-vm bloquea el disco (flock) para que dos instancias no lo
  abran a la vez, porque eso corrompería NTFS.
- **Archivos compartidos con Aurora (fase D):** tarjeta de red virtual Intel
  e1000 (Windows trae el driver) con NAT, que también da Internet a las apps.
  AuroraOS sirve `/home/<usuario>` por SMB (Samba de los paquetes de Ubuntu) y
  el agente lo conecta como `Z:` al arrancar. Así, abrir un .docx desde Archivos
  de Aurora lanza la app de Windows con `Z:\ruta\fichero.docx`, y lo guardado
  aparece al momento en Aurora.
- **Carpetas del usuario unificadas:** Descargas, Documentos, Escritorio,
  Imágenes, Música y Vídeos de Windows son las mismas que las de Aurora, no copias.
  El agente redirige las carpetas del perfil de Windows a la carpeta compartida,
  igual que hace la redirección de carpetas de las empresas. Escribe en
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders`
  las claves `{374DE290-123F-4565-9164-39C4925E467B}` (Descargas), `Personal`,
  `Desktop`, `My Pictures`, `My Music` y `My Video`, apuntando a `Z:\Descargas`, etc.
  Los nombres reales los pide a aurora-vm, que los lee de `~/.config/user-dirs.dirs`.
  Lo que una app de Windows descarga cae directamente en `~/Descargas` de Aurora,
  y lo que pones ahí desde Aurora se ve en el explorador de archivos de Windows.
- **Avisos de cambios:** Samba avisa a Windows de los cambios (inotify en AuroraOS),
  así que las carpetas abiertas se refrescan solas.
- **Tras reanudar:** la unidad `Z:` se reconecta sola. El agente comprueba que está
  conectada antes de lanzar ninguna app.
- **Velocidad del disco (más adelante):** cambiar el IDE emulado por AHCI
  (Windows trae el driver) para tener colas de órdenes y menos salidas por operación.

## Orden y pruebas

1. **A1:** desfase del reloj + TSC_OFFSET y guardado/carga del estado. Prueba en
   el banco Unicorn con el CD de ReactOS: suspender en el escritorio y reanudar.
2. **A2:** suspensión automática al cerrar la ventana (`--hibernar`) y reanudación
   al arrancar si hay snapshot.
3. **B:** 16550 + agente (ReactOS en el banco y luego Windows 10).
4. **C:** ventanas sueltas.
5. **D:** integración.
