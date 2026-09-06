#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT     2
void *heap_caps_realloc(void *ptr, size_t size, unsigned caps);
