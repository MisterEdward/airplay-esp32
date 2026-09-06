#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT     2
#define MALLOC_CAP_SPIRAM   4
void *heap_caps_realloc(void *ptr, size_t size, unsigned caps);
void *heap_caps_malloc(size_t size, unsigned caps);
void heap_caps_free(void *ptr);
