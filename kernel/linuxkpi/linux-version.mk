# Versiones fijadas de las fuentes externas de la capa LinuxKPI.
# Se lee desde make (include) y desde bash (source): solo VAR=valor, sin espacios.
# No cambiar sin discutirlo (ver Plan_LinuxKPI.md, seccion de decisiones).

# Linux: serie 6.18 LTS. LINUX_VERSION es la version exacta que se descarga:
# el ultimo 6.18.y publicado el 26-oct-2026 (kernel.org/releases.json).
LINUX_SERIES=6.18
LINUX_VERSION=6.18.59

# linux-firmware: etiqueta de fecha (AAAAMMDD) del repositorio linux-firmware
# (la mas reciente en cdn.kernel.org el 26-oct-2026; no hace falta hasta L4).
LINUX_FIRMWARE_TAG=20261019

# open-gpu-kernel-modules de NVIDIA. Debe coincidir con el firmware GSP y con
# el userspace de NVIDIA (Plan_NVIDIA.md).
NVIDIA_OPEN_VERSION=615.78.08
