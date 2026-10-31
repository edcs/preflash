#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)

#ifdef __cplusplus
extern "C" {
#endif

void* heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void* ptr);
// The host has no separate memory regions, so every capability reports the
// same fixed figure rather than a live one.
size_t heap_caps_get_free_size(uint32_t caps);

#ifdef __cplusplus
}
#endif
