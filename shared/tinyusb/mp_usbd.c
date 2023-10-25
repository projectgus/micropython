/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Blake W. Felt & Angus Gratton
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

#include <stdlib.h>

#include "py/mpconfig.h"
#include "py/runtime.h"

#if MICROPY_HW_ENABLE_USBDEV

#ifndef NO_QSTR
#include "tusb.h" // TinyUSB is not available when running the string preprocessor
#include "device/dcd.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#endif

#include "pico/stdlib.h"

#define IO_DBG1 2
#define IO_DBG2 3

// Legacy TinyUSB task function wrapper, called by some ports from the interpreter hook
void usbd_task(void) {
    tud_task_ext(0, false);
}

// TinyUSB task function wrapper, as scheduled from the USB IRQ
static void mp_usbd_task(mp_sched_node_t *node);

extern void __real_dcd_event_handler(dcd_event_t const *event, bool in_isr);

// If -Wl,--wrap=dcd_event_handler is passed to the linker, then this wrapper
// will be called and allows MicroPython to schedule the TinyUSB task when
// dcd_event_handler() is called from an ISR.
TU_ATTR_FAST_FUNC void __wrap_dcd_event_handler(dcd_event_t const *event, bool in_isr) {
    static mp_sched_node_t usbd_task_node;

    __real_dcd_event_handler(event, in_isr);
    static bool init;
    if (MP_UNLIKELY(!init)) {
        gpio_init(IO_DBG1);
        gpio_set_dir(IO_DBG1, GPIO_OUT);
        gpio_init(IO_DBG2);
        gpio_set_dir(IO_DBG2, GPIO_OUT);
        init = true;
    }
    gpio_put(IO_DBG2, !gpio_get(IO_DBG2));
    if (usbd_task_node.callback == NULL) {
        mp_sched_schedule_node(&usbd_task_node, mp_usbd_task);
    }
}

static void mp_usbd_task(mp_sched_node_t *node) {
    (void)node;
    gpio_put(IO_DBG1, 1);
    tud_task_ext(0, false);
    gpio_put(IO_DBG1, 0);
}


#endif
