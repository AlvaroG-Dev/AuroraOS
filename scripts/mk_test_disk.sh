#!/bin/bash
# Crea una imagen con GPT + 3 particiones para probar el partition layer.
set -e

IMG="tests/test_disk.img"
SIZE="${1:-64M}"

mkdir -p tests
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"

# Requiere gdisk (sgdisk). En Debian/Ubuntu: apt install gdisk
sgdisk \
  --new=1:2048:+16M --typecode=1:ef00 --change-name=1:"EFI System" \
  --new=2:0:+16M    --typecode=2:8300 --change-name=2:"Linux data" \
  --new=3:0:0       --typecode=3:8200 --change-name=3:"Linux swap" \
  "$IMG"

echo "OK: $IMG creada"
sgdisk -p "$IMG"