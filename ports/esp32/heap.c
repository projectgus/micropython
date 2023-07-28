/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Angus Gratton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include "py/gc.h"
#include "py/misc.h"
#include "py/mpstate.h"
#include "heap.h"
#include "esp_heap_caps.h"
#include "heap_memory_layout.h"

// Start and end of the MicroPython heap
void *mp_task_heap, *mp_task_heap_end;

// Capability flags used to allocate the heap
#define ALLOC_HEAP_CAPS (MALLOC_CAP_DEFAULT)

#define PREFER_PSRAM_THRESH 1024

// The actual caps used to allocate the python heap
STATIC uint32_t py_heap_caps;

// Link-time wrappers for heap functions, these allocate from ESP-IDF directly
void *__real_heap_caps_malloc(size_t size, uint32_t caps);
void *__real_heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *__real_heap_caps_realloc( void *ptr, size_t size, uint32_t caps);
void __real_heap_caps_free( void *ptr);
void *__real_heap_caps_malloc_default(size_t size);
void *__real_heap_caps_realloc_default(void *ptr, size_t size);

// Return true if ptr is in the Python heap
STATIC inline bool ptr_in_py_heap(void *ptr)
{
  return mp_task_heap && (ptr >= mp_task_heap) && (ptr < mp_task_heap_end);
}

// Return true if caps are compatible with the Python heap
// (i.e. all bits in 'caps' are also set in py_heap_caps)
//
// Will return false if py_heap_caps is zero (i.e. mp_alloc_heap not called yet).
STATIC inline bool caps_for_py_heap(uint32_t caps)
{
    return (caps & py_heap_caps) == caps;
}

// Return the full set of capabilities for a particular RAM address.
//
// The esp_heap_caps.h API doesn't seem to include this functionality, even
// though heap_private.h has the necessary support functions to calculate it.
//
// Likely this won't work if addr is in IRAM mapped D/IRAM, but that's fine here
// - we only need it to find the Python heap's memory capabilities.
STATIC uint32_t caps_for_addr(intptr_t addr)
{
    const soc_memory_type_desc_t *type = NULL;
    uint32_t caps = 0;

    // Find the region that 'addr' is in, extract the memory type
    for (int i = 0; i < soc_memory_region_count; i++) {
        const soc_memory_region_t *region = &soc_memory_regions[i];
        if (region->start <= addr && region->start + region->size > addr) {
            type = &soc_memory_types[region->type];
            break;
        }
    }

    if (type == NULL) {
        //printf("Failed to find region of %p\n", (void *)addr);
        return 0;
    }

    // Calculate the total capabilities of this memory type
    for (int i = 0; i < SOC_MEMORY_TYPE_NO_PRIOS; i++) {
        caps |= type->caps[i];
    }

    return caps;
}

void mp_alloc_heap(void)
{
    // Allocate the uPy heap using malloc and get the largest available region,
    // less some overhead for heap metadata.
    //
    // When SPIRAM is enabled, this will allocate from SPIRAM.
    const size_t HEAP_OVERHEAD = 256;
    size_t mp_task_heap_size = heap_caps_get_largest_free_block(ALLOC_HEAP_CAPS) - HEAP_OVERHEAD;
    mp_task_heap = __real_heap_caps_malloc(mp_task_heap_size, ALLOC_HEAP_CAPS);
    mp_task_heap_end = mp_task_heap + mp_task_heap_size;
    py_heap_caps = caps_for_addr((intptr_t)mp_task_heap);

    //printf("Heap allocated at %p size %zu caps 0x%"PRIx32"\n", mp_task_heap, mp_task_heap_size, py_heap_caps);
}

IRAM_ATTR void *__wrap_heap_caps_malloc(size_t size, uint32_t caps)
{
    void *result = NULL;
    bool py_first = false;

    #ifdef CONFIG_SPIRAM
    if (size > PREFER_PSRAM_THRESH && caps_for_py_heap(caps)) {
        // TODO: check if calls to calloc() end up here, wasteful to have to zero buffer twice
        result = m_tracked_calloc(1, size);
        if (result) {
            return result;
        }
        py_first = true;
    }
    #endif

    if (result == NULL) {
        result = __real_heap_caps_malloc(size, caps);
    }

    if (!py_first && result == NULL && caps_for_py_heap(caps)) {
        result = m_tracked_calloc(1, size);
    }

    return result;
}

IRAM_ATTR void *__wrap_heap_caps_calloc(size_t nmemb, size_t size, uint32_t caps)
{
    void *result = NULL;
    bool py_first = false;

    #ifdef CONFIG_SPIRAM
    if (size > PREFER_PSRAM_THRESH && caps_for_py_heap(caps)) {
        result = m_tracked_calloc(nmemb, size);
        if (result) {
            return result;
        }
        py_first = true;
    }
    #endif

    if (result == NULL) {
        result = __real_heap_caps_calloc(nmemb, size, caps);
    }

    if (!py_first && result == NULL && caps_for_py_heap(caps)) {
        result = m_tracked_calloc(nmemb, size);
    }

    return result;
}

IRAM_ATTR void *__wrap_heap_caps_realloc( void *ptr, size_t size, uint32_t caps)
{
    if (size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }

    if (ptr_in_py_heap(ptr)) {
        abort(); // TODO: implement m_tracked_realloc() for this

        return NULL;
    }
    else {
        // Pointer is in IDF heap (or NULL). If necessary, the inner
        // heap_caps_realloc() will fall back to manually calling
        // heap_caps_malloc(), then copy, then heap_caps_free() the old buffer -
        // so the IDF implementation should still allow for the buffer to be
        // moved into the Python heap if that is the only space for it.
        return __real_heap_caps_realloc(ptr, size, caps);
    }
}

IRAM_ATTR void __wrap_heap_caps_free( void *ptr)
{
    if (ptr_in_py_heap(ptr)) {
        m_tracked_free(ptr);
    } else {
        __real_heap_caps_free(ptr);
    }
}

IRAM_ATTR void *__wrap_heap_caps_malloc_default(size_t size)
{
    // With PSRAM enabled, the ESP-IDF __real_heap_caps_malloc_default tries to
    // preference allocations from internal or external memory first, based on a
    // size threshold.
    //
    // There's not really any point calling into that here, because if PSRAM is
    // enabled then we've used all of it for the Python heap.
    //
    // So try to allocate with MALLOC_CAP_DEFAULT caps - this will effectively
    // try to allocate from the ESP-IDF heap (internal memory available only)
    // first, and fail over to the Python heap if it fails.
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

IRAM_ATTR void *__wrap_heap_caps_realloc_default(void *ptr, size_t size)
{
    // Similar to the rationale above, we can call straight through
    // to heap_caps_realloc() here
    return heap_caps_realloc(ptr, size, MALLOC_CAP_DEFAULT);
}
