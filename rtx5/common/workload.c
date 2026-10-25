/*
 * Copyright 2025-2026 BKPT, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "cmsis_os2.h"
#include CMSIS_device_header
#include "ViewAlyzer.h"
#include "ViewAlyzerRTX5Hook.h"
#include <stdint.h>
#include <string.h>

/* User traces and ISR names share the recorder's registration table. */
enum { RAW_CYCLES_ID = 60, KERNEL_TICKS_ID = 61, ROUND_ID = 62 };
_Static_assert(RAW_CYCLES_ID > 11 && RAW_CYCLES_ID != TIM2_IRQn + 16 &&
               KERNEL_TICKS_ID != TIM2_IRQn + 16 && ROUND_ID != TIM2_IRQn + 16,
               "Timing markers must not overwrite kernel or TIM2 ISR names");

volatile uint32_t rtx5_example_round;
volatile uint32_t rtx5_example_failures;
volatile uint32_t rtx5_example_timer_fires;
volatile uint32_t rtx5_example_completed;
volatile uint32_t rtx5_example_lifecycle_done;
volatile uint32_t rtx5_example_irq_rounds;
volatile uint32_t rtx5_example_fp_rounds[2];
volatile uint32_t rtx5_example_fp_exits;
static osMessageQueueId_t queue;
static osMessageQueueId_t limits_queue;
static osSemaphoreId_t semaphore;
static osSemaphoreId_t limits_semaphore;
static osMutexId_t mutex;
static osEventFlagsId_t flags;
static osMemoryPoolId_t pool;
static osTimerId_t timer;
static osMessageQueueId_t irq_queue;
static osSemaphoreId_t irq_semaphore, irq_ack;
static osEventFlagsId_t irq_flags;
static osMemoryPoolId_t irq_pool;
static osThreadId_t consumer_thread, irq_thread;

static void expect(int32_t actual, int32_t wanted)
{
    if (actual != wanted) {
        ++rtx5_example_failures;
        VA_LogString(2, "RTX5:unexpected-status");
    }
}

static void timer_callback(void *argument)
{
    (void)argument;
    VA_RTX5_TimerEnter(timer);
    ++rtx5_example_timer_fires;
    VA_RTX5_TimerExit(timer);
}

void TIM2_IRQHandler(void)
{
    VA_LogISRStart(TIM2_IRQn + 16);
    uint32_t value = rtx5_example_irq_rounds + 1;
    expect(osMessageQueuePut(irq_queue, &value, 0, 0), osOK);
    expect((int32_t)osEventFlagsSet(irq_flags, 1), 1);
    void *block = osMemoryPoolAlloc(irq_pool, 0);
    if (block) expect(osMemoryPoolFree(irq_pool, block), osOK);
    else ++rtx5_example_failures;
    expect(osSemaphoreRelease(irq_semaphore), osOK);
    expect((int32_t)osThreadFlagsSet(irq_thread, 0x10), 0x10);
    /* These illegal ISR calls must clear their trace request without
       inventing a successful operation or consuming a request slot. */
    expect((int32_t)osThreadFlagsClear(1), (int32_t)osFlagsErrorISR);
    expect((int32_t)osThreadFlagsWait(1, osFlagsWaitAny, 0), (int32_t)osFlagsErrorISR);
    VA_LogISREnd(TIM2_IRQn + 16);
}

static void irq_consumer(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t value;
        expect((int32_t)osThreadFlagsWait(0x10, osFlagsWaitAll, osWaitForever), 0x10);
        expect(osSemaphoreAcquire(irq_semaphore, osWaitForever), osOK);
        expect(osMessageQueueGet(irq_queue, &value, 0, 0), osOK);
        expect((int32_t)osEventFlagsWait(irq_flags, 1, osFlagsWaitAll, 0), 1);
        expect((int32_t)value, (int32_t)(rtx5_example_irq_rounds + 1));
        ++rtx5_example_irq_rounds;
        expect(osSemaphoreRelease(irq_ack), osOK);
    }
}

static void floating_point_worker(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    uint32_t wanted[16], actual[16];
    for (unsigned i = 0; i < 16; ++i) wanted[i] = 0x3f800000u + (index << 16) + i;
    for (;;) {
        /* Exercise all callee-saved FP registers across a real context
           switch; different tasks load different bit patterns. */
        __asm volatile("vldmia %0, {s16-s31}" :: "r"(wanted) :
            "s16","s17","s18","s19","s20","s21","s22","s23",
            "s24","s25","s26","s27","s28","s29","s30","s31","memory");
        expect(osDelay(37 + 4 * index), osOK);
        __asm volatile("vstmia %0, {s16-s31}" :: "r"(actual) : "memory");
        if (memcmp(wanted, actual, sizeof(wanted)) != 0) ++rtx5_example_failures;
        ++rtx5_example_fp_rounds[index];
    }
}

static void consumer(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t value = 0;
        expect(osMessageQueueGet(queue, &value, 0, osWaitForever), osOK);
        expect(osMutexAcquire(mutex, osWaitForever), osOK);
        expect(osMutexRelease(mutex), osOK);
        expect(osSemaphoreAcquire(semaphore, osWaitForever), osOK);
        expect((int32_t)osEventFlagsWait(flags, 1, osFlagsWaitAll, osWaitForever), 1);
        expect((int32_t)osThreadFlagsWait(3, osFlagsWaitAll, osWaitForever), 3);
        ++rtx5_example_completed;
    }
}

static void disposable(void *argument)
{
    (void)argument;
    __asm volatile("vmov s0, %0" :: "r"(0x3f800000u) : "s0");
    expect(osThreadSuspend(osThreadGetId()), osOK);
    expect(osDelay(2), osOK);
}

static void detached_fp_exit(void *argument)
{
    (void)argument;
    __asm volatile("vmov s0, %0" :: "r"(0x40000000u) : "s0");
    ++rtx5_example_fp_exits;
    osThreadExit();
}

static void cancelled_flags(void *argument)
{
    (void)argument;
    /* The controller terminates this detached waiter; cleanup must not read
       its freed control block while closing the outstanding wait. */
    (void)osThreadFlagsWait(0x100, osFlagsWaitAll, osWaitForever);
    ++rtx5_example_failures;
}

static void suspended_flags(void *argument)
{
    (void)argument;
    expect((int32_t)osThreadFlagsWait(0x200, osFlagsWaitAll, osWaitForever),
        (int32_t)osFlagsErrorTimeout);
}

static void lifecycle(void)
{
    const osThreadAttr_t suspended = {.name = "suspend flags", .priority = osPriorityAboveNormal,
        .stack_size = 2048};
    osThreadId_t suspended_waiter = osThreadNew(suspended_flags, 0, &suspended);
    if (suspended_waiter) {
        expect(osThreadSuspend(suspended_waiter), osOK);
        expect(osThreadResume(suspended_waiter), osOK);
    } else ++rtx5_example_failures;
    const osThreadAttr_t cancelled = {.name = "cancel flags", .priority = osPriorityAboveNormal,
        .stack_size = 2048};
    osThreadId_t waiter = osThreadNew(cancelled_flags, 0, &cancelled);
    if (waiter) expect(osThreadTerminate(waiter), osOK); else ++rtx5_example_failures;
    const osThreadAttr_t detached = {.name = "FP exit", .priority = osPriorityAboveNormal,
        .stack_size = 2048};
    if (!osThreadNew(detached_fp_exit, 0, &detached)) ++rtx5_example_failures;
    const osThreadAttr_t attr = {.name = "joinable", .attr_bits = osThreadJoinable,
        .priority = osPriorityAboveNormal, .stack_size = 2048};
    osThreadId_t child = osThreadNew(disposable, 0, &attr);
    if (!child) { ++rtx5_example_failures; return; }
    expect(osThreadResume(child), osOK);
    expect(osThreadJoin(child), osOK);
    child = osThreadNew(disposable, 0, &attr);
    if (!child) { ++rtx5_example_failures; return; }
    expect(osThreadTerminate(child), osOK);
    expect(osThreadJoin(child), osOK);
    const osMutexAttr_t ma = {.name = "deleted mutex", .attr_bits = osMutexPrioInherit};
    osMutexId_t deleted_mutex = osMutexNew(&ma);
    if (deleted_mutex) {
        expect(osMutexAcquire(deleted_mutex, 0), osOK);
        expect(osMutexDelete(deleted_mutex), osOK);
    } else ++rtx5_example_failures;
    osSemaphoreId_t deleted_sem = osSemaphoreNew(1, 0, 0);
    osMessageQueueId_t deleted_queue = osMessageQueueNew(1, 4, 0);
    osEventFlagsId_t deleted_flags = osEventFlagsNew(0);
    osMemoryPoolId_t deleted_pool = osMemoryPoolNew(1, 16, 0);
    osTimerId_t deleted_timer = osTimerNew(timer_callback, osTimerOnce, 0, 0);
    if (deleted_sem) expect(osSemaphoreDelete(deleted_sem), osOK); else ++rtx5_example_failures;
    if (deleted_queue) expect(osMessageQueueDelete(deleted_queue), osOK); else ++rtx5_example_failures;
    if (deleted_flags) expect(osEventFlagsDelete(deleted_flags), osOK); else ++rtx5_example_failures;
    if (deleted_pool) expect(osMemoryPoolDelete(deleted_pool), osOK); else ++rtx5_example_failures;
    if (deleted_timer) expect(osTimerDelete(deleted_timer), osOK); else ++rtx5_example_failures;
    ++rtx5_example_lifecycle_done;
}

static void resource_limits(void)
{
    uint32_t value = 42, received = 0;
    expect(osMessageQueuePut(limits_queue, &value, 0, 0), osOK);
    expect(osMessageQueuePut(limits_queue, &value, 0, 0), osErrorResource);
    expect(osMessageQueuePut(limits_queue, &value, 0, 1), osErrorTimeout);
    expect(osMessageQueueGet(limits_queue, &received, 0, 0), osOK);
    expect((int32_t)received, 42);
    expect(osMessageQueueGet(limits_queue, &received, 0, 0), osErrorResource);
    expect(osMessageQueueGet(limits_queue, &received, 0, 1), osErrorTimeout);
    expect(osMessageQueuePut(limits_queue, &value, 0, 0), osOK);
    expect(osMessageQueueReset(limits_queue), osOK);
    expect(osSemaphoreAcquire(limits_semaphore, 0), osErrorResource);
    expect(osSemaphoreAcquire(limits_semaphore, 1), osErrorTimeout);
    expect(osSemaphoreRelease(limits_semaphore), osOK);
    expect(osSemaphoreRelease(limits_semaphore), osErrorResource);
    expect(osSemaphoreAcquire(limits_semaphore, 0), osOK);
    void *first = osMemoryPoolAlloc(pool, 0);
    void *second = osMemoryPoolAlloc(pool, 0);
    if (!first || !second) { ++rtx5_example_failures; return; }
    if (osMemoryPoolAlloc(pool, 0) || osMemoryPoolAlloc(pool, 1)) ++rtx5_example_failures;
    expect(osMemoryPoolFree(pool, first), osOK);
    expect(osMemoryPoolFree(pool, second), osOK);
    expect((int32_t)osEventFlagsWait(flags, 2, osFlagsWaitAll, 0), (int32_t)osFlagsErrorResource);
    expect((int32_t)osEventFlagsWait(flags, 2, osFlagsWaitAll, 1), (int32_t)osFlagsErrorTimeout);
    expect((int32_t)osEventFlagsSet(flags, 2), 2);
    expect((int32_t)osEventFlagsClear(flags, 2), 2);
    osThreadId_t self = osThreadGetId();
    expect((int32_t)osThreadFlagsSet(self, 0x30), 0x30);
    expect((int32_t)osThreadFlagsWait(0x10, osFlagsWaitAny | osFlagsNoClear, 0), 0x30);
    expect((int32_t)osThreadFlagsClear(0x10), 0x30);
    expect((int32_t)osThreadFlagsGet(), 0x20);
    expect((int32_t)osThreadFlagsWait(0x20, osFlagsWaitAll, 0), 0x20);
    expect((int32_t)osThreadFlagsWait(0x40, osFlagsWaitAny, 0), (int32_t)osFlagsErrorResource);
    expect((int32_t)osThreadFlagsWait(0x40, osFlagsWaitAll | osFlagsNoClear, 1), (int32_t)osFlagsErrorTimeout);
    expect((int32_t)osThreadFlagsSet(self, 0x80000000u), (int32_t)osFlagsErrorParameter);
}

static void controller(void *argument)
{
    (void)argument;
    /* Allow the host to enter Recording before one-shot lifecycle coverage. */
    expect(osDelay(1500), osOK);
    lifecycle();
    expect(osTimerStart(timer, 10), osOK);
    for (;;) {
        uint32_t value = ++rtx5_example_round;
        if ((value & 63u) == 0) {
            expect(osTimerStop(timer), osOK);
            expect(osTimerStart(timer, 10), osOK);
        }
        resource_limits();
        expect(osMutexAcquire(mutex, 0), osOK);
        expect(osMutexAcquire(mutex, 0), osOK);
        expect(osMutexRelease(mutex), osOK);
        /* The consumer now blocks on our mutex and boosts us. */
        expect(osMessageQueuePut(queue, &value, 0, osWaitForever), osOK);
        expect((int32_t)osThreadGetPriority(osThreadGetId()), osPriorityAboveNormal);
        expect(osMutexRelease(mutex), osOK);
        expect((int32_t)osThreadGetPriority(osThreadGetId()), osPriorityNormal);
        expect(osSemaphoreRelease(semaphore), osOK);
        /* The higher-priority waiter consumes the bit before Set returns. */
        expect((int32_t)osEventFlagsSet(flags, 1), 0);
        expect((int32_t)osThreadFlagsSet(consumer_thread, 1), 1);
        expect((int32_t)osThreadFlagsSet(consumer_thread, 2), 0);
        NVIC_SetPendingIRQ(TIM2_IRQn);
        expect(osSemaphoreAcquire(irq_ack, 10), osOK);
        VA_TickOverflowCheck();
        VA_Drain();
        VA_LogTrace(RAW_CYCLES_ID, (int32_t)DWT->CYCCNT);
        VA_LogTrace(KERNEL_TICKS_ID, (int32_t)osKernelGetTickCount());
        VA_LogTrace(ROUND_ID, (int32_t)value);
        /* Keep sustained trace traffic below a remotely forwarded probe's
           drain rate while repeating every scenario throughout capture. */
        expect(osDelay(80), osOK);
    }
}

void rtx5_workload_init(void)
{
    const osMessageQueueAttr_t qa = {.name = "transfer"};
    const osSemaphoreAttr_t sa = {.name = "handshake"};
    const osMessageQueueAttr_t lqa = {.name = "queue limits"};
    const osSemaphoreAttr_t lsa = {.name = "sem limits"};
    const osMutexAttr_t ma = {.name = "recursive", .attr_bits = osMutexRecursive | osMutexPrioInherit};
    const osEventFlagsAttr_t fa = {.name = "signals"};
    const osMemoryPoolAttr_t pa = {.name = "blocks"};
    const osTimerAttr_t ta = {.name = "periodic"};
    const osThreadAttr_t ca = {.name = "controller", .priority = osPriorityNormal, .stack_size = 2048};
    const osThreadAttr_t wa = {.name = "consumer", .priority = osPriorityAboveNormal, .stack_size = 2048};
    queue = osMessageQueueNew(2, sizeof(uint32_t), &qa);
    limits_queue = osMessageQueueNew(1, sizeof(uint32_t), &lqa);
    semaphore = osSemaphoreNew(1, 0, &sa);
    limits_semaphore = osSemaphoreNew(1, 0, &lsa);
    mutex = osMutexNew(&ma);
    flags = osEventFlagsNew(&fa);
    pool = osMemoryPoolNew(2, 32, &pa);
    timer = osTimerNew(timer_callback, osTimerPeriodic, 0, &ta);
    const osMessageQueueAttr_t iqa = {.name = "IRQ queue"};
    const osSemaphoreAttr_t isa = {.name = "IRQ semaphore"}, iaa = {.name = "IRQ ack"};
    const osEventFlagsAttr_t ifa = {.name = "IRQ flags"};
    const osMemoryPoolAttr_t ipa = {.name = "IRQ blocks"};
    irq_queue = osMessageQueueNew(2, sizeof(uint32_t), &iqa);
    irq_semaphore = osSemaphoreNew(1, 0, &isa);
    irq_ack = osSemaphoreNew(1, 0, &iaa);
    irq_flags = osEventFlagsNew(&ifa);
    irq_pool = osMemoryPoolNew(2, 32, &ipa);
    if (!queue || !limits_queue || !semaphore || !limits_semaphore || !mutex || !flags || !pool || !timer ||
        !irq_queue || !irq_semaphore || !irq_ack || !irq_flags || !irq_pool) {
        ++rtx5_example_failures;
        return;
    }
    consumer_thread = osThreadNew(consumer, 0, &wa);
    if (!consumer_thread || !osThreadNew(controller, 0, &ca))
        ++rtx5_example_failures;
    const osThreadAttr_t ia = {.name = "IRQ consumer", .priority = osPriorityAboveNormal1, .stack_size = 2048};
    const osThreadAttr_t fpa = {.name = "FP A", .priority = osPriorityBelowNormal, .stack_size = 2048};
    const osThreadAttr_t fpb = {.name = "FP B", .priority = osPriorityBelowNormal, .stack_size = 2048};
    irq_thread = osThreadNew(irq_consumer, 0, &ia);
    if (!irq_thread || !osThreadNew(floating_point_worker, 0, &fpa) ||
        !osThreadNew(floating_point_worker, (void *)1, &fpb)) ++rtx5_example_failures;
    VA_RegisterUserTrace(TIM2_IRQn + 16, "TIM2 IRQ", VA_USER_TYPE_ISR);
    VA_RegisterUserTrace(RAW_CYCLES_ID, "RawCycles", VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(KERNEL_TICKS_ID, "KernelTicks", VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(ROUND_ID, "Round", VA_USER_TYPE_COUNTER);
    NVIC_SetPriority(TIM2_IRQn, 8);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
}
