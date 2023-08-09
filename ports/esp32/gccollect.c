/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Damien P. George
 * Copyright (c) 2017 Pycom Limited
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

#include <stdio.h>

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/gc.h"
#include "py/mpthread.h"
#include "gccollect.h"

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3

#include "xtensa/hal.h"

static void gc_collect_inner(int level) {
    if (level < XCHAL_NUM_AREGS / 8) {
        gc_collect_inner(level + 1);
        if (level != 0) {
            return;
        }
    }

    if (level == XCHAL_NUM_AREGS / 8) {
        // get the sp
        volatile uint32_t sp = (uint32_t)esp_cpu_get_sp();
        gc_collect_root((void **)sp, ((mp_uint_t)MP_STATE_THREAD(stack_top) - sp) / sizeof(uint32_t));
        return;
    }

    // trace root pointers from any threads
    #if MICROPY_PY_THREAD
    mp_thread_gc_others();
    #endif
}

void gc_collect(void) {
    gc_collect_start();
    gc_collect_inner(0);
    gc_collect_end();
}

#elif CONFIG_IDF_TARGET_ESP32C3

#include "shared/runtime/gchelper.h"

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    #if MICROPY_PY_THREAD
    mp_thread_gc_others();
    #endif
    gc_collect_end();
}

#endif

#if MICROPY_GC_SPLIT_HEAP_AUTO

// Unless necessary to avoid a Python MemoryError,
// try to reserve this much free system heap for ESP-IDF
#define RESERVE_SYSTEM_HEAP (24 * 1024)

size_t gc_get_max_new_split(size_t needed) {
    multi_heap_info_t info = { 0 };
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

    // The largest new region that is available to become Python heap is the largest
    // free block in the ESP-IDF system heap...
    size_t max = info.largest_free_block;

    if (max <= needed) {
        return max;
    }

    // ... unless overall free heap is running low, in which case
    // prefer to save some RAM for ESP-IDF unless it's needed.
    if (info.total_free_bytes < max + RESERVE_SYSTEM_HEAP) {
        if (max > needed + RESERVE_SYSTEM_HEAP) {
            // Reserve some of the largest free block for the system. New Python
            // heap area will still be big enough for 'needed'.
            max -= RESERVE_SYSTEM_HEAP;
        } else {
            // Memory is really low, only allow Python exactly what it needs
            max = needed;
        }
    }

    return max;
}

#endif
