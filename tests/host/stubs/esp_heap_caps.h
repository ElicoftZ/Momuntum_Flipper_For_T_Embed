#pragma once
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DEFAULT 4U

void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps);
