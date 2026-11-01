# Pruebas de usuarios en hardware real (U0–U4)

Rama: `claude/plan-usuarios`. Estas pruebas **no tocan la GPU ni el escritorio**: el sistema arranca como siempre (`aur.login=root` por defecto) y todo se hace desde la terminal de Aurora. Verificado en QEMU; aquí se comprueba lo que QEMU no reproduce (USB real, caché, apagado).

## 0. Compilar y arrancar

```
git fetch origin claude/plan-usuarios && git checkout claude/plan-usuarios
make iso WITH_NVIDIA=0          # en la Portégé no hace falta NVIDIA
```
Graba/arranca la ISO como siempre. Todos los comandos siguientes se escriben en la **terminal de Aurora** (arranca como root).

## 1. Estado inicial
```
id                        # esperado: uid=0(root) gid=0(root) ...
ls /data                  # /data debe estar montado (si no, la prueba 2 no vale)
```

## 2. Propietarios y modos en /data (persisten tras reiniciar)
```
mkdir -p /data/prueba && touch /data/prueba/f
chmod 640 /data/prueba/f
chown 1000:1000 /data/prueba/f
ls -ln /data/prueba/f     # esperado: -rw-r----- 1 1000 1000 ...
sync
reboot
```
Tras volver a arrancar:
```
ls -ln /data/prueba/f     # DEBE seguir siendo -rw-r----- 1 1000 1000
ls -a /data               # aparece .aurora-meta (y .aurora-meta.new)
```
Si sale `-rw-r--r-- 1 0 0` los metadatos no se guardaron: **dímelo con el log** (`dmesg | grep -i "FAT32\|aurora-meta"`).

## 3. Cuentas, contraseñas, su y sudo
```
useradd -m -G wheel pepe  # esperado: "useradd: creado pepe (uid=1001 ...)"
passwd pepe               # escribe una clave dos veces
passwd alvaro             # fija también una clave para alvaro (está bloqueado)
id pepe                   # esperado: uid=1001(pepe) ... groups=...,10(wheel)
ls -ld /data/home/pepe    # esperado: drwx------ ... pepe pepe
su pepe                   # sin clave (eres root)
  id                      # uid=1001(pepe)
  cat /etc/shadow         # debe DENEGARSE (Permission denied)
  sudo id                 # uid=0(root), sin pedir clave (LiveUSB)
  su alvaro               # pide clave; con la correcta entra, con una mala falla
  exit
exit                      # vuelves a root (si no, cierra el último shell)
```

## 4. /etc persistente
```
sync
reboot
```
Tras reiniciar:
```
id pepe                   # debe seguir existiendo
grep pepe /etc/passwd
ls -l /etc/shadow         # esperado: -rw------- root root
su pepe -c id             # sigue funcionando
```

## 5. (Opcional) Terminal con login automático como alvaro
Antes de compilar, en `bootloader/aurora.conf` cambia la línea
`cmdline=nvidia-drm.vblank=1` por `cmdline=nvidia-drm.vblank=1 aur.login=auto:alvaro` (en la Portégé basta `cmdline=aur.login=auto:alvaro`), recompila y arranca. En la terminal:
```
id                        # esperado: uid=1000(alvaro)
touch /etc/x              # debe fallar (Permission denied)
sudo id                   # uid=0 (alvaro está en wheel)
```
**Aviso:** con esta opción los programas gráficos lanzados desde ese shell aún no conectan con el escritorio (llega con U5). Para volver atrás, quita `aur.login=...`.

## 6. Sesión gráfica como usuario (U5a/U5b)
En `bootloader/aurora.conf` **añade** al final de tu `cmdline=` actual (no borres lo que ya tengas):
`aur.session=user:alvaro`
Ejemplo Portégé: `cmdline=aur.session=user:alvaro`. Ejemplo 4060: `cmdline=nvidia-drm.vblank=1 aur.session=user:alvaro`.
Antes de arrancar (una vez, como root): `printf 'video:x:44:alvaro\ninput:x:104:alvaro\nrender:x:105:alvaro\n' >> /etc/group && sync`

Pruebas (en la terminal que se abre, ya como alvaro):
1. `id` → `uid=1000(alvaro)`, con grupos `wheel video input render`.
2. `echo $XDG_RUNTIME_DIR; ls -l /run/user/1000` → `/run/user/1000`, con `wayland-0`.
3. Escritorio: ¿arranca, se mueve el ratón y responde el teclado? (`ls -l /dev/input/event0` debe ser grupo 104).
4. Abrir una app gráfica desde el Dock y otra desde la terminal (p. ej. `xterm`). Si falla, mira `ps` y `dmesg | tail -60`.
5. `touch /etc/x` (debe fallar), `sudo id` (uid=0) y, tras `sync` y reiniciar, `id` sigue igual.
Volver atrás: quita `aur.session=...` y todo vuelve a root.

## Qué enviarme si algo falla
Salida del comando que falla y `dmesg | tail -60` (o la foto del F12). Un apagado brusco sin `sync`/`poweroff` pierde los cambios de la sesión: es lo esperado.
