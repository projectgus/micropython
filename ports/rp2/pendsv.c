/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Damien P. George
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

#include <assert.h>
#include "py/mpconfig.h"
#include "py/mpthread.h"
#include "pendsv.h"
#include "shared/runtime/softtimer.h"

#if PICO_RP2040
#include "RP2040.h"
#elif PICO_RP2350 && PICO_ARM
#include "RP2350.h"
#elif PICO_RISCV
#include "pico/aon_timer.h"
#endif

#if MICROPY_PY_NETWORK_CYW43
#include "lib/cyw43-driver/src/cyw43_stats.h"
#endif

static pendsv_dispatch_t pendsv_dispatch_table[PENDSV_DISPATCH_NUM_SLOTS];

static inline void pendsv_resume_run_dispatch(void);

void PendSV_Handler(void);

#if MICROPY_PY_THREAD

// Important to use a 'nowait' mutex here as softtimer updates PendSV from the
// loop of mp_wfe_or_timeout(), where we don't want the CPU event bit to be set.
static mp_thread_recursive_mutex_t pendsv_mutex;

// Provide a static soft timer node as a cross-core trigger from CPU1 to CPU0 PendSV
//
// "If pendsv_schedule_dispatch() is only called from core0 then why is this needed?"
// The reason is the following interleaving:
//
// core1 calls pendsv_suspend() - for example to queue a softtimer
// core0 triggers PendSV
// core0 PendSV_Handler sees it is suspended and exits to run later
// core1 calls pendsv_resume() - sets core1_trigger which will fire on core0
// core0 soft timer IRQ triggers PendSV on core0
// core0 handles the PendSV that was triggered earlier
soft_timer_entry_t core1_trigger;

void pendsv_init(void) {
    mp_thread_recursive_mutex_init(&pendsv_mutex);
    // Note we only support running PendSV IRQ on CPU0, so it's
    // unconfigured on CPU1.
    #if !defined(__riscv)
    NVIC_SetPriority(PendSV_IRQn, IRQ_PRI_PENDSV);
    #endif

    // Note the timer doesn't have an associated callback, it just exists to create a
    // hardware interrupt to wake CPU0
    soft_timer_static_init(&core1_trigger, SOFT_TIMER_MODE_ONE_SHOT, 0, NULL);
}

void pendsv_suspend(void) {
    // Recursive Mutex here as either core may call pendsv_suspend() and expect
    // both mutual exclusion (other core can't enter pendsv_suspend() at the
    // same time), and that no PendSV handler will run.
    mp_thread_recursive_mutex_lock(&pendsv_mutex, 1);
}

void pendsv_resume(void) {
    mp_thread_recursive_mutex_unlock(&pendsv_mutex);
    pendsv_resume_run_dispatch();
}

static inline int pendsv_suspend_count(void) {
    return pendsv_mutex.mutex.enter_count;
}

static inline void pendsv_arm_trigger(void) {
    if (get_core_num() == 0) {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    } else {
        // We want PendSV IRQ to only run on CPU0, so queue a soft timer interrupt
        // (which will run on CPU0 and trigger a PendSV there)
        soft_timer_reinsert(&core1_trigger, 0);
    }
}

#else

// Without threads we don't include any pico-sdk mutex in the build,
// but also we don't need to worry about cross-thread contention (or
// races with interrupts that update this counter).
static int pendsv_lock;

void pendsv_init(void) {
}

void pendsv_suspend(void) {
    pendsv_lock++;
}

void pendsv_resume(void) {
    assert(pendsv_lock > 0);
    pendsv_lock--;
    pendsv_resume_run_dispatch();
}

static inline int pendsv_suspend_count(void) {
    return pendsv_lock;
}

static inline void pendsv_arm_trigger(void) {
    assert(get_core_num() == 0);
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

#endif

static inline void pendsv_resume_run_dispatch(void) {
    // Run pendsv if needed.  Find an entry with a dispatch and call pendsv dispatch
    // with it.  If pendsv runs it will service all slots.
    int count = PENDSV_DISPATCH_NUM_SLOTS;
    while (count--) {
        if (pendsv_dispatch_table[count]) {
            pendsv_schedule_dispatch(count, pendsv_dispatch_table[count]);
            break;
        }
    }
}

void pendsv_schedule_dispatch(size_t slot, pendsv_dispatch_t f) {
    pendsv_dispatch_table[slot] = f;
    if (pendsv_suspend_count() == 0) {
        // There is a race here where other core calls pendsv_suspend() before
        // ISR can execute, but dispatch will happen later when other core
        // calls pendsv_resume().
        #if PICO_ARM
        pendsv_arm_trigger();
        #elif PICO_RISCV
        struct timespec ts;
        aon_timer_get_time(&ts);
        aon_timer_enable_alarm(&ts, PendSV_Handler, false);
        #endif
    } else {
        #if MICROPY_PY_NETWORK_CYW43
        CYW43_STAT_INC(PENDSV_DISABLED_COUNT);
        #endif
    }
}

// PendSV interrupt handler to perform background processing.
void PendSV_Handler(void) {
    assert(get_core_num() == 0);

    #if MICROPY_PY_THREAD
    if (!mp_thread_recursive_mutex_lock(&pendsv_mutex, 0)) {
        // Failure here means core 1 holds pendsv_mutex. ISR will
        // run again after core 1 calls pendsv_resume().
        return;
    }
    // Core 0 should not already have locked pendsv_mutex
    assert(pendsv_mutex.mutex.enter_count == 1);
    #else
    assert(pendsv_suspend_count() == 0);
    #endif

    #if MICROPY_PY_NETWORK_CYW43
    CYW43_STAT_INC(PENDSV_RUN_COUNT);
    #endif

    for (size_t i = 0; i < PENDSV_DISPATCH_NUM_SLOTS; ++i) {
        if (pendsv_dispatch_table[i] != NULL) {
            pendsv_dispatch_t f = pendsv_dispatch_table[i];
            pendsv_dispatch_table[i] = NULL;
            f();
        }
    }

    #if MICROPY_PY_THREAD
    mp_thread_recursive_mutex_unlock(&pendsv_mutex);
    #endif
}
