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

#if MICROPY_HW_ENABLE_USBDEV

#ifndef NO_QSTR
#include "tusb.h" // TinyUSB is not available when running the string preprocessor
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#endif

#include "pico/stdlib.h"

#define IO_DBG1 2
#define IO_DBG2 3

static void TU_ATTR_FAST_FUNC _toggle_dbg2(void) {
    gpio_put(IO_DBG2, !gpio_get(IO_DBG2));
}

void usbd_task(void) {
    static bool init;
    if (MP_UNLIKELY(!init)) {
        gpio_init(IO_DBG1);
        gpio_set_dir(IO_DBG1, GPIO_OUT);
        gpio_init(IO_DBG2);
        gpio_set_dir(IO_DBG2, GPIO_OUT);
        irq_add_shared_handler(USBCTRL_IRQ, _toggle_dbg2, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
        init = true;
    }
    gpio_put(IO_DBG1, 1);
    tud_task_ext(0, false);
    gpio_put(IO_DBG1, 0);
}

#endif
