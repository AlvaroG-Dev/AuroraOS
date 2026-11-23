#!/bin/bash
# Disco de prueba para la mini-VM: tests/test_disk.img con la misma GPT que
# mk_test_disk.sh, pero de 45 GB y con la tercera particion en ext4 (FAT32 no
# admite ficheros de mas de 4 GB). Dentro deja la ISO como windows.iso y un disco
# virtual vacio win.img (disperso: no ocupa hasta que la VM escribe).
#
#   scripts/mk_vm_disk.sh RUTA/A/windows.iso [TAMANO_IMAGEN] [TAMANO_DISCO_VM]
#   make vm-disk ISO=RUTA/A/windows.iso
#
# En AuroraOS:
#   mount -t ext4 /dev/sda3 /data
#   aurora-vm --bios --ide /data/win.img --cd /data/windows.iso --mem 3072
#
# No necesita root: el ext4 se genera con mkfs.ext4 -d y se copia en su sitio.
# Requiere gdisk (sgdisk) y e2fsprogs.
set -e

ISO="$1"
SIZE="${2:-45G}"
VMDISK="${3:-40G}"
IMG="tests/test_disk.img"

if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
  echo "uso: $0 RUTA/A/windows.iso [tamano imagen, 45G] [tamano disco VM, 40G]" >&2
  exit 1
fi
for t in sgdisk mkfs.ext4; do
  command -v "$t" >/dev/null || { echo "falta $t (apt install gdisk e2fsprogs)" >&2; exit 1; }
done

mkdir -p tests
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"
sgdisk \
  --new=1:2048:+16M --typecode=1:ef00 --change-name=1:"EFI System" \
  --new=2:0:+16M    --typecode=2:8300 --change-name=2:"Linux data" \
  --new=3:0:0       --typecode=3:8300 --change-name=3:"VM" \
  "$IMG" >/dev/null

start=$(sgdisk -i 3 "$IMG" | awk '/First sector/ {print $3}')
end=$(sgdisk -i 3 "$IMG" | awk '/Last sector/ {print $3}')
bytes=$(( (end - start + 1) * 512 ))

stage=$(mktemp -d tests/vmdisk.XXXXXX)
part="$stage.p3"
trap 'rm -rf "$stage" "$part"' EXIT
cp --reflink=auto "$ISO" "$stage/windows.iso"
truncate -s "$VMDISK" "$stage/win.img"

# Sin orphan_file ni metadata_csum_seed: con ellos AuroraOS monta el ext4 solo
# en lectura (kernel/ext2.c, EXT2_*_RW_OK). e2fsprogs viejos no conocen
# orphan_file: entonces se repite sin el.
truncate -s "$bytes" "$part"
if ! mkfs.ext4 -q -F -L VM -O ^orphan_file,^metadata_csum_seed -d "$stage" "$part" 2>/dev/null; then
  mkfs.ext4 -q -F -L VM -O ^metadata_csum_seed -d "$stage" "$part"
fi
dd if="$part" of="$IMG" bs=4M seek=$((start * 512)) oflag=seek_bytes conv=notrunc,sparse status=none

echo "OK: $IMG ($SIZE) con /dev/sda3 en ext4: windows.iso + win.img ($VMDISK)"
sgdisk -p "$IMG"
