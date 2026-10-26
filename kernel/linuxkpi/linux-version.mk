# Versiones fijadas de las fuentes externas de la capa LinuxKPI.
# Se lee desde make (include) y desde bash (source): solo VAR=valor, sin espacios.
# No cambiar sin discutirlo (ver Plan_LinuxKPI.md, seccion de decisiones).

# Linux: serie 6.18 LTS. LINUX_VERSION es la version exacta que se descarga
# (provisional: la 6.18 base, hasta poder consultar el ultimo 6.18.y).
LINUX_SERIES=6.18
LINUX_VERSION=6.18

# linux-firmware: etiqueta de fecha (AAAAMMDD) del repositorio linux-firmware.
# Vacio = sin fijar todavia (no hace falta hasta L4).
LINUX_FIRMWARE_TAG=

# open-gpu-kernel-modules de NVIDIA. Debe coincidir con el firmware GSP y con
# el userspace de NVIDIA (Plan_NVIDIA.md).
NVIDIA_OPEN_VERSION=615.78.08
