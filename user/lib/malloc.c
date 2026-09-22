#include "malloc.h"
#include "string.h"
#include "../syscall.h"

#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define CHUNK_SIZE 0x10000 // 64 KB por petición sbrk para reducir syscalls

typedef struct block_header {
  size_t size;               // Tamaño usable del bloque
  int is_free;               // 1 = libre, 0 = ocupado
  struct block_header *next; // Siguiente bloque
} block_header_t;

#define HEADER_SIZE ALIGN(sizeof(block_header_t))

static block_header_t *free_list_head = NULL;

static block_header_t *request_space(block_header_t *last, size_t size) {
  size_t total_needed = size + HEADER_SIZE;
  size_t alloc_size = (total_needed < CHUNK_SIZE) ? CHUNK_SIZE : ALIGN(total_needed);

  void *request = sys_sbrk((intptr_t)alloc_size);
  if (request == (void *)-1) {
    return NULL;
  }

  block_header_t *block = (block_header_t *)request;
  block->size = alloc_size - HEADER_SIZE;
  block->is_free = 0;
  block->next = NULL;

  if (last) {
    last->next = block;
  }

  return block;
}

void *malloc(size_t size) {
  if (size == 0) return NULL;
  size = ALIGN(size);

  block_header_t *current = free_list_head;
  block_header_t *last = NULL;

  // 1. First-fit search
  while (current) {
    if (current->is_free && current->size >= size) {
      // Split block if large enough
      if (current->size >= size + HEADER_SIZE + ALIGNMENT) {
        block_header_t *new_block = (block_header_t *)((uint8_t *)current + HEADER_SIZE + size);
        new_block->size = current->size - size - HEADER_SIZE;
        new_block->is_free = 1;
        new_block->next = current->next;

        current->size = size;
        current->next = new_block;
      }
      current->is_free = 0;
      return (void *)((uint8_t *)current + HEADER_SIZE);
    }
    last = current;
    current = current->next;
  }

  // 2. No free block found, request from kernel via sbrk
  block_header_t *block = request_space(last, size);
  if (!block) return NULL;

  if (!free_list_head) {
    free_list_head = block;
  }

  // If newly requested block is larger than needed, split it
  if (block->size >= size + HEADER_SIZE + ALIGNMENT) {
    block_header_t *new_block = (block_header_t *)((uint8_t *)block + HEADER_SIZE + size);
    new_block->size = block->size - size - HEADER_SIZE;
    new_block->is_free = 1;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
  }

  return (void *)((uint8_t *)block + HEADER_SIZE);
}

void free(void *ptr) {
  if (!ptr) return;

  block_header_t *block = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
  block->is_free = 1;

  // Coalesce free blocks
  block_header_t *curr = free_list_head;
  while (curr && curr->next) {
    if (curr->is_free && curr->next->is_free) {
      curr->size += HEADER_SIZE + curr->next->size;
      curr->next = curr->next->next;
    } else {
      curr = curr->next;
    }
  }
}

void *calloc(size_t num, size_t size) {
  if (num != 0 && size > (size_t)-1 / num)
    return NULL;

  size_t total = num * size;
  void *ptr = malloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

void *realloc(void *ptr, size_t size) {
  if (!ptr) return malloc(size);
  if (size == 0) {
    free(ptr);
    return NULL;
  }

  size = ALIGN(size);
  block_header_t *block = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
  if (block->size >= size) {
    return ptr;
  }

  void *new_ptr = malloc(size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
  }
  return new_ptr;
}
