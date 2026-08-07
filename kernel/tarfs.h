// kernel/tarfs.h
#ifndef TARFS_H
#define TARFS_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    char name[256];      // Ruta normalizada (ej: "system/icons/terminal.bmp")
    uint8_t *data;       // Puntero directo al inicio del contenido en RAM
    size_t size;         // Tamaño del archivo en bytes
    int is_dir;          // 1 = Directorio, 0 = Archivo
} tar_node_t;

// Inicializa el Initramfs analizando el bloque TAR
void tarfs_init(const void *tar_addr, size_t tar_size);

// Abre/busca un archivo o carpeta por su ruta
tar_node_t *tarfs_open(const char *path);

// Imprime el contenido del directorio por serial para debug
void tarfs_list(const char *dir_path);

// Funciones auxiliares de consulta
size_t tarfs_get_node_count(void);
tar_node_t *tarfs_get_node(size_t index);
tar_node_t *tar_find_file(const char *path);

#endif