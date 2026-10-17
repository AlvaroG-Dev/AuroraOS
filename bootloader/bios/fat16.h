/* bootloader/bios/fat16.h */
#ifndef AURORA_BIOS_FAT16_H
#define AURORA_BIOS_FAT16_H
#include "bios.h"

int fat16_mount_iso(uint8_t drive, uint32_t iso_lba_2048,
                    uint32_t iso_size_bytes);
int fat16_mount_disk(uint8_t drive, uint32_t part_lba_512,
                     uint32_t part_size_bytes);

int fat16_find(const char *path_8_3, fat_file_t *out);
int fat16_read(uint8_t drive, const fat_file_t *f, uint32_t off, uint32_t len,
               void *buf);

uint32_t fat16_data_start_lba(void);
uint32_t fat16_cluster_size(void);

#endif