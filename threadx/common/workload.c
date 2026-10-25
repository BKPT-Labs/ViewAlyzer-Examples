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
/* STM32 ThreadX recorder exercise. ST supplies startup, vectors, the linker
 * script, system initialization and the selected Cortex-M tick port.
 * Data caches remain disabled so debug-port readers observe RAM/RTT writes. */
#if defined(STM32F767xx)
#include "stm32f7xx.h"
#else
#include "board_device.h"
#endif
#include "tx_api.h"
#include "ViewAlyzer.h"
#include "VA_Adapter_ThreadX.h"
#define TX_EXAMPLE_STRING_INNER(value) #value
#define TX_EXAMPLE_STRING(value) TX_EXAMPLE_STRING_INNER(value)
#define TX_EXAMPLE_VERSION TX_EXAMPLE_STRING(THREADX_MAJOR_VERSION) "." \
    TX_EXAMPLE_STRING(THREADX_MINOR_VERSION) "." TX_EXAMPLE_STRING(THREADX_PATCH_VERSION)

static TX_THREAD controller, helper, disposable;
static ULONG controller_stack[1024], helper_stack[1024], disposable_stack[512];
static TX_SEMAPHORE command_sem, done_sem, semaphore, isr_sem, delete_sem, owner_ready;
static TX_QUEUE queue, isr_queue, late_queue;
static TX_MUTEX mutex, owner_mutex, priority_mutex;
static TX_EVENT_FLAGS_GROUP flags, isr_flags, late_flags;
static TX_BYTE_POOL byte_pool;
static TX_BLOCK_POOL block_pool;
static TX_TIMER timer;
static ULONG queue_memory[2], byte_memory[256], block_memory[80];
static ULONG isr_queue_memory[1];
static ULONG late_queue_memory[1];
static volatile UINT command, disposable_wait, isr_done;
volatile ULONG threadx_example_round;
volatile ULONG threadx_example_failures;
volatile ULONG threadx_example_timer_fires;
volatile ULONG threadx_example_isr_rounds;

enum { RECEIVE, SEMAPHORE, MUTEX, FLAGS, BYTE, BLOCK, SEND, FRONT_SEND, DELETE_WAIT,
       OWNER_MUTEX, PRIORITY_MUTEX, DELETE_RECEIVE, DELETE_SEND, DELETE_FRONT,
       DELETE_FLAGS, WAKE = 0x100 };

static void expect(UINT actual, UINT wanted)
{
    if (actual != wanted) {
        ++threadx_example_failures;
        VA_LogString(2, "THREADX:unexpected-status");
    }
}

#if defined(STM32F767xx)
static void clock_init(void)
{
    /* HSI is board-local; unlike HSE bypass it needs no ST-Link MCO cable. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR1 |= PWR_CR1_VOS;
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {}
    RCC->PLLCFGR = 16u | (432u << RCC_PLLCFGR_PLLN_Pos) | (9u << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}
    PWR->CR1 |= PWR_CR1_ODEN;
    while (!(PWR->CSR1 & PWR_CSR1_ODRDY)) {}
    PWR->CR1 |= PWR_CR1_ODSWEN;
    while (!(PWR->CSR1 & PWR_CSR1_ODSWRDY)) {}
    FLASH->ACR = FLASH_ACR_LATENCY_7WS | FLASH_ACR_PRFTEN;
    RCC->CFGR = RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}
    SystemCoreClockUpdate();
    /* PB3 AF0 is the asynchronous SWO output. Host config owns TPI/ITM. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 6)) | (2u << 6);
    GPIOB->OSPEEDR |= 3u << 6;
    GPIOB->AFR[0] &= ~(15u << 12);
    DBGMCU->CR |= DBGMCU_CR_TRACE_IOEN;
}
#endif

static void timer_callback(ULONG unused)
{
    (void)unused;
    VA_ThreadX_TimerEnter(&timer);
    ++threadx_example_timer_fires;
    VA_LogString(3, "THREADX:timer-callback");
    VA_ThreadX_TimerExit(&timer);
}

static void disposable_entry(ULONG unused)
{
    (void)unused;
    if (disposable_wait) {
        expect(tx_mutex_get(&owner_mutex, TX_NO_WAIT), TX_SUCCESS);
        expect(tx_semaphore_put(&owner_ready), TX_SUCCESS);
        expect(tx_thread_sleep(100), TX_SUCCESS);
    }
    /* First entry returns; the reset instance blocks and is terminated. */
    VA_LogString(5, "THREADX:disposable-complete");
}

/* Pend the NVIC directly: no physical EXTI input or peripheral configuration
 * is needed. This IRQ has one explicit hook pair and no context-save wrapper. */
void EXTI0_IRQHandler(void)
{
    ULONG value = 99;
    VA_ThreadX_ISR_Enter();
    expect(tx_queue_send(&isr_queue, &value, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_semaphore_put(&isr_sem), TX_SUCCESS);
    expect(tx_event_flags_set(&isr_flags, 1, TX_OR), TX_SUCCESS);
    isr_done = 1;
    VA_ThreadX_ISR_Exit();
}

static void helper_entry(ULONG unused)
{
    ULONG value;
    VOID *memory;
    (void)unused;
    for (;;) {
        expect(tx_semaphore_get(&command_sem, TX_WAIT_FOREVER), TX_SUCCESS);
        const UINT waking = command & WAKE;
        const ULONG wait = waking ? TX_WAIT_FOREVER : 2;
        switch (command & ~WAKE) {
        case SEND:
            value = 106;
            expect(tx_queue_send(&queue, &value, wait), waking ? TX_SUCCESS : TX_QUEUE_FULL);
            break;
        case FRONT_SEND:
            value = 107;
            expect(tx_queue_front_send(&queue, &value, wait), waking ? TX_SUCCESS : TX_QUEUE_FULL);
            break;
        case RECEIVE:
            expect(tx_queue_receive(&queue, &value, wait), waking ? TX_SUCCESS : TX_QUEUE_EMPTY);
            break;
        case SEMAPHORE:
            expect(tx_semaphore_get(&semaphore, wait), waking ? TX_SUCCESS : TX_NO_INSTANCE);
            break;
        case MUTEX:
            expect(tx_mutex_get(&mutex, wait), waking ? TX_SUCCESS : TX_NOT_AVAILABLE);
            if (waking) expect(tx_mutex_put(&mutex), TX_SUCCESS);
            break;
        case FLAGS:
            expect(tx_event_flags_get(&flags, 0x80, TX_OR_CLEAR, &value, wait), waking ? TX_SUCCESS : TX_NO_EVENTS);
            break;
        case BYTE:
            expect(tx_byte_allocate(&byte_pool, &memory, 256, wait), waking ? TX_SUCCESS : TX_NO_MEMORY);
            if (waking) expect(tx_byte_release(memory), TX_SUCCESS);
            break;
        case BLOCK:
            expect(tx_block_allocate(&block_pool, &memory, wait), waking ? TX_SUCCESS : TX_NO_MEMORY);
            if (waking) expect(tx_block_release(memory), TX_SUCCESS);
            break;
        case DELETE_WAIT:
            expect(tx_semaphore_get(&delete_sem, TX_WAIT_FOREVER), TX_DELETED);
            break;
        case DELETE_RECEIVE:
            expect(tx_queue_receive(&late_queue, &value, TX_WAIT_FOREVER), TX_DELETED);
            break;
        case DELETE_SEND:
            value = 108;
            expect(tx_queue_send(&late_queue, &value, TX_WAIT_FOREVER), TX_DELETED);
            break;
        case DELETE_FRONT:
            value = 109;
            expect(tx_queue_front_send(&late_queue, &value, TX_WAIT_FOREVER), TX_DELETED);
            break;
        case DELETE_FLAGS:
            expect(tx_event_flags_get(&late_flags, 3, TX_AND_CLEAR, &value, TX_WAIT_FOREVER), TX_DELETED);
            break;
        case OWNER_MUTEX:
            expect(tx_mutex_get(&owner_mutex, TX_WAIT_FOREVER), TX_SUCCESS);
            expect(tx_mutex_put(&owner_mutex), TX_SUCCESS);
            break;
        case PRIORITY_MUTEX:
            VA_LogString(5, "THREADX:priority-peer");
            expect(tx_mutex_get(&priority_mutex, TX_WAIT_FOREVER), TX_SUCCESS);
            expect(tx_mutex_put(&priority_mutex), TX_SUCCESS);
            break;
        }
        expect(tx_semaphore_put(&done_sem), TX_SUCCESS);
    }
}

static void waiting_case(UINT which)
{
    /* Helper priority 6 preempts controller priority 8. It must block inside
     * this operation before command_sem's give returns to the controller.
     * The subsequent give/send then wakes a higher-priority receiver. */
    command = which | WAKE;
    expect(tx_semaphore_put(&command_sem), TX_SUCCESS);
    /* Check the actual wait list, not just the API result after the wake. */
    switch (which) {
    case SEND: case FRONT_SEND: case RECEIVE:
        expect(queue.tx_queue_suspended_count, 1); break;
    case SEMAPHORE:
        expect(semaphore.tx_semaphore_suspended_count, 1); break;
    case MUTEX:
        expect(mutex.tx_mutex_suspended_count, 1);
        expect(controller.tx_thread_priority, 6); break;
    case FLAGS:
        expect(flags.tx_event_flags_group_suspended_count, 1); break;
    case BYTE:
        expect(byte_pool.tx_byte_pool_suspended_count, 1); break;
    case BLOCK:
        expect(block_pool.tx_block_pool_suspended_count, 1); break;
    case DELETE_WAIT:
        expect(delete_sem.tx_semaphore_suspended_count, 1); break;
    case DELETE_RECEIVE: case DELETE_SEND: case DELETE_FRONT:
        expect(late_queue.tx_queue_suspended_count, 1); break;
    case DELETE_FLAGS:
        expect(late_flags.tx_event_flags_group_suspended_count, 1); break;
    case OWNER_MUTEX:
        expect(owner_mutex.tx_mutex_suspended_count, 1);
        expect(disposable.tx_thread_priority, 6); break;
    }
}

static void finish_waiting_case(void)
{
    expect(tx_semaphore_get(&done_sem, TX_WAIT_FOREVER), TX_SUCCESS);
}

static void blocking_case(UINT which)
{
    command = which;
    expect(tx_semaphore_put(&command_sem), TX_SUCCESS);
    expect(tx_semaphore_get(&done_sem, TX_WAIT_FOREVER), TX_SUCCESS);
}

static void deleted_waiters(void)
{
    /* Block at high priority, then lower the waiter. Deletion and same-address
       reuse must finish before it returns TX_DELETED from the old object. */
    for (UINT which = DELETE_RECEIVE; which <= DELETE_FLAGS; ++which) {
        UINT priority, ignored;
        ULONG value = 1;
        if (which == DELETE_FLAGS)
            expect(tx_event_flags_create(&late_flags, "TxLateFlags"), TX_SUCCESS);
        else {
            const char *name = which == DELETE_RECEIVE ? "TxLateReceive" :
                which == DELETE_SEND ? "TxLateSend" : "TxLateFront";
            expect(tx_queue_create(&late_queue, (CHAR *)name, TX_1_ULONG,
                late_queue_memory, sizeof late_queue_memory), TX_SUCCESS);
            if (which != DELETE_RECEIVE) expect(tx_queue_send(&late_queue, &value, TX_NO_WAIT), TX_SUCCESS);
        }
        waiting_case(which);
        expect(tx_thread_priority_change(&helper, 12, &priority), TX_SUCCESS);
        if (which == DELETE_FLAGS) {
            expect(tx_event_flags_delete(&late_flags), TX_SUCCESS);
            expect(tx_event_flags_create(&late_flags, "TxReusedFlags"), TX_SUCCESS);
        } else {
            expect(tx_queue_delete(&late_queue), TX_SUCCESS);
            expect(tx_queue_create(&late_queue, "TxReusedQueue", TX_1_ULONG,
                late_queue_memory, sizeof late_queue_memory), TX_SUCCESS);
        }
        finish_waiting_case();
        expect(tx_thread_priority_change(&helper, priority, &ignored), TX_SUCCESS);
        if (which == DELETE_FLAGS) expect(tx_event_flags_delete(&late_flags), TX_SUCCESS);
        else expect(tx_queue_delete(&late_queue), TX_SUCCESS);
    }
    UINT priority, ignored;
    expect(tx_semaphore_create(&delete_sem, "TxLateSem", 0), TX_SUCCESS);
    waiting_case(DELETE_WAIT);
    expect(tx_thread_priority_change(&helper, 12, &priority), TX_SUCCESS);
    expect(tx_semaphore_delete(&delete_sem), TX_SUCCESS);
    expect(tx_semaphore_create(&delete_sem, "TxReusedSem", 1), TX_SUCCESS);
    finish_waiting_case();
    expect(delete_sem.tx_semaphore_count, 1);
    expect(tx_thread_priority_change(&helper, priority, &ignored), TX_SUCCESS);
    expect(tx_semaphore_delete(&delete_sem), TX_SUCCESS);
    VA_LogString(5, "THREADX:late-delete-complete");
}

static void run_round(void)
{
    ULONG value = 42, actual = 0;
    UINT old;
    VOID *memory = TX_NULL, *blocks[16];
    UINT count = 0;
    VA_LogString(1, "THREADX:round-begin");
    VA_LogString(4, "THREADX:kernel:" TX_EXAMPLE_VERSION);
    VA_LogString(4, "THREADX:coverage:v2");
    /* Let RAM readers acknowledge their initial stream checkpoint before
       the one-time deletion/reuse case; later rounds keep normal load. */
    if (threadx_example_round == 1) deleted_waiters();
    expect(tx_queue_create(&queue, "TxQueue", TX_1_ULONG, queue_memory, sizeof queue_memory), TX_SUCCESS);
    expect(tx_semaphore_create(&semaphore, "TxSemaphore", 0), TX_SUCCESS);
    expect(tx_mutex_create(&mutex, "TxMutex", TX_INHERIT), TX_SUCCESS);
    expect(tx_event_flags_create(&flags, "TxFlags"), TX_SUCCESS);
    expect(tx_byte_pool_create(&byte_pool, "TxBytes", byte_memory, sizeof byte_memory), TX_SUCCESS);
    expect(tx_block_pool_create(&block_pool, "TxBlocks", 64, block_memory, sizeof block_memory), TX_SUCCESS);
    expect(tx_timer_create(&timer, "TxTimer", timer_callback, 0, 1, 2, TX_AUTO_ACTIVATE), TX_SUCCESS);
    expect(tx_queue_send(&queue, &value, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_queue_front_send(&queue, &value, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_queue_send(&queue, &value, TX_NO_WAIT), TX_QUEUE_FULL);
    blocking_case(SEND);
    blocking_case(FRONT_SEND);
    waiting_case(SEND);
    expect(tx_queue_receive(&queue, &actual, TX_NO_WAIT), TX_SUCCESS);
    expect(actual, 42);
    finish_waiting_case();
    waiting_case(FRONT_SEND);
    expect(tx_queue_receive(&queue, &actual, TX_NO_WAIT), TX_SUCCESS);
    expect(actual, 107);
    finish_waiting_case();
    expect(tx_queue_receive(&queue, &actual, TX_NO_WAIT), TX_SUCCESS);
    expect(actual, 42);
    expect(tx_queue_flush(&queue), TX_SUCCESS);
    expect(tx_queue_receive(&queue, &actual, TX_NO_WAIT), TX_QUEUE_EMPTY);
    blocking_case(RECEIVE);
    waiting_case(RECEIVE);
    expect(tx_queue_send(&queue, &value, TX_NO_WAIT), TX_SUCCESS);
    finish_waiting_case();
    expect(tx_semaphore_put(&semaphore), TX_SUCCESS);
    expect(tx_semaphore_ceiling_put(&semaphore, 1), TX_CEILING_EXCEEDED);
    expect(tx_semaphore_get(&semaphore, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_semaphore_get(&semaphore, TX_NO_WAIT), TX_NO_INSTANCE);
    expect(tx_semaphore_ceiling_put(&semaphore, 1), TX_SUCCESS);
    expect(tx_semaphore_get(&semaphore, TX_NO_WAIT), TX_SUCCESS);
    blocking_case(SEMAPHORE);
    waiting_case(SEMAPHORE);
    expect(tx_semaphore_put(&semaphore), TX_SUCCESS);
    finish_waiting_case();
    expect(tx_mutex_get(&mutex, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_mutex_get(&mutex, TX_NO_WAIT), TX_SUCCESS);
    blocking_case(MUTEX);
    expect(tx_mutex_put(&mutex), TX_SUCCESS);
    /* A timeout does not restore the inherited owner priority. Release the
     * recursive hold completely, then acquire once for the real handoff. */
    expect(tx_mutex_put(&mutex), TX_SUCCESS);
    expect(controller.tx_thread_priority, 8);
    expect(tx_mutex_get(&mutex, TX_NO_WAIT), TX_SUCCESS);
    waiting_case(MUTEX);
    expect(tx_mutex_put(&mutex), TX_SUCCESS);
    finish_waiting_case();
    expect(tx_mutex_put(&mutex), TX_NOT_OWNED);
    expect(tx_event_flags_set(&flags, 3, TX_OR), TX_SUCCESS);
    expect(tx_event_flags_get(&flags, 3, TX_AND, &actual, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_event_flags_get(&flags, 1, TX_OR_CLEAR, &actual, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_event_flags_set(&flags, 0, TX_AND), TX_SUCCESS);
    expect(tx_event_flags_get(&flags, 1, TX_OR, &actual, TX_NO_WAIT), TX_NO_EVENTS);
    blocking_case(FLAGS);
    waiting_case(FLAGS);
    expect(tx_event_flags_set(&flags, 0x80, TX_OR), TX_SUCCESS);
    finish_waiting_case();
    expect(tx_byte_allocate(&byte_pool, &memory, 64, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_byte_release(memory), TX_SUCCESS);
    expect(tx_byte_allocate(&byte_pool, &memory, 800, TX_NO_WAIT), TX_SUCCESS);
    VOID *unavailable = TX_NULL;
    expect(tx_byte_allocate(&byte_pool, &unavailable, 256, TX_NO_WAIT), TX_NO_MEMORY);
    blocking_case(BYTE);
    waiting_case(BYTE);
    expect(tx_byte_release(memory), TX_SUCCESS);
    finish_waiting_case();
    while (count < 16 && tx_block_allocate(&block_pool, &blocks[count], TX_NO_WAIT) == TX_SUCCESS) ++count;
    expect(count, 4);
    if (count != 4) {
        while (count) expect(tx_block_release(blocks[--count]), TX_SUCCESS);
        goto cleanup;
    }
    blocking_case(BLOCK);
    waiting_case(BLOCK);
    expect(tx_block_release(blocks[--count]), TX_SUCCESS);
    finish_waiting_case();
    while (count) expect(tx_block_release(blocks[--count]), TX_SUCCESS);
    expect(tx_thread_priority_change(&helper, 11, &old), TX_SUCCESS);
    expect(tx_thread_priority_change(&helper, old, &old), TX_SUCCESS);
    expect(tx_thread_suspend(&helper), TX_SUCCESS);
    expect(tx_thread_resume(&helper), TX_SUSPEND_LIFTED);
    expect(tx_mutex_create(&priority_mutex, "TxPriorityMutex", TX_INHERIT), TX_SUCCESS);
    expect(tx_mutex_get(&priority_mutex, TX_NO_WAIT), TX_SUCCESS);
    UINT helper_priority, controller_priority;
    expect(tx_thread_priority_change(&helper, 12, &helper_priority), TX_SUCCESS);
    command = PRIORITY_MUTEX | WAKE;
    expect(tx_semaphore_put(&command_sem), TX_SUCCESS);
    expect(helper.tx_thread_state, TX_READY);
    VA_LogString(5, "THREADX:priority-begin");
    /* The ready helper must preempt inside this API, then contend before the
     * caller returns. The resulting inheritance is distinct from demotion. */
    expect(tx_thread_priority_change(&controller, 13, &controller_priority), TX_SUCCESS);
    expect(controller_priority, 8);
    expect(priority_mutex.tx_mutex_suspended_count, 1);
    expect(controller.tx_thread_priority, 12);
    expect(controller.tx_thread_user_priority, 13);
    VA_LogString(5, "THREADX:priority-return");
    expect(tx_mutex_put(&priority_mutex), TX_SUCCESS);
    expect(tx_thread_priority_change(&controller, controller_priority, &old), TX_SUCCESS);
    expect(tx_thread_priority_change(&helper, helper_priority, &old), TX_SUCCESS);
    finish_waiting_case();
    expect(tx_mutex_delete(&priority_mutex), TX_SUCCESS);
    VA_LogString(5, "THREADX:priority-complete");
    expect(tx_mutex_create(&owner_mutex, "TxOwnerMutex", TX_INHERIT), TX_SUCCESS);
    expect(tx_semaphore_create(&owner_ready, "TxOwnerReady", 0), TX_SUCCESS);
    disposable_wait = 0;
    expect(tx_thread_create(&disposable, "TxDisposable", disposable_entry, 0,
        disposable_stack, sizeof disposable_stack, 12, 12, TX_NO_TIME_SLICE, TX_DONT_START), TX_SUCCESS);
    expect(tx_thread_resume(&disposable), TX_SUCCESS);
    expect(tx_thread_suspend(&disposable), TX_SUCCESS);
    expect(tx_thread_resume(&disposable), TX_SUCCESS);
    expect(tx_thread_priority_change(&disposable, 13, &old), TX_SUCCESS);
    expect(tx_thread_priority_change(&disposable, 12, &old), TX_SUCCESS);
    expect(tx_thread_priority_change(&disposable, 8, &old), TX_SUCCESS);
    VA_LogString(5, "THREADX:yield-begin");
    tx_thread_relinquish();
    VA_LogString(5, "THREADX:yield-end");
    expect(disposable.tx_thread_state, TX_COMPLETED);
    disposable_wait = 1;
    expect(tx_thread_reset(&disposable), TX_SUCCESS);
    expect(tx_thread_resume(&disposable), TX_SUCCESS);
    expect(tx_semaphore_get(&owner_ready, TX_WAIT_FOREVER), TX_SUCCESS);
    waiting_case(OWNER_MUTEX);
    expect(tx_thread_terminate(&disposable), TX_SUCCESS);
    finish_waiting_case();
    expect(tx_thread_delete(&disposable), TX_SUCCESS);
    expect(tx_mutex_delete(&owner_mutex), TX_SUCCESS);
    expect(tx_semaphore_delete(&owner_ready), TX_SUCCESS);
    VA_LogString(5, "THREADX:lifecycle-complete");
    expect(tx_semaphore_create(&delete_sem, "TxDeleteSem", 0), TX_SUCCESS);
    waiting_case(DELETE_WAIT);
    expect(tx_semaphore_delete(&delete_sem), TX_SUCCESS);
    finish_waiting_case();
    VA_LogString(5, "THREADX:delete-waiter-complete");
    expect(tx_queue_create(&isr_queue, "TxIsrQueue", TX_1_ULONG, isr_queue_memory, sizeof isr_queue_memory), TX_SUCCESS);
    expect(tx_semaphore_create(&isr_sem, "TxIsrSem", 0), TX_SUCCESS);
    expect(tx_event_flags_create(&isr_flags, "TxIsrFlags"), TX_SUCCESS);
    isr_done = 0;
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_SetPriority(EXTI0_IRQn, 3);
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_SetPendingIRQ(EXTI0_IRQn);
    __DSB(); __ISB();
    /* Failure remains bounded if the IRQ unexpectedly cannot execute. */
    for (unsigned spin = 0; !isr_done && spin < 10000; ++spin) __NOP();
    NVIC_DisableIRQ(EXTI0_IRQn);
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    expect(isr_done, 1);
    expect(tx_queue_receive(&isr_queue, &actual, TX_NO_WAIT), TX_SUCCESS);
    expect(actual, 99);
    expect(tx_semaphore_get(&isr_sem, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_event_flags_get(&isr_flags, 1, TX_OR_CLEAR, &actual, TX_NO_WAIT), TX_SUCCESS);
    expect(tx_queue_delete(&isr_queue), TX_SUCCESS);
    expect(tx_semaphore_delete(&isr_sem), TX_SUCCESS);
    expect(tx_event_flags_delete(&isr_flags), TX_SUCCESS);
    ++threadx_example_isr_rounds;
    VA_LogString(5, "THREADX:isr-complete");
    expect(tx_timer_deactivate(&timer), TX_SUCCESS);
    expect(tx_timer_change(&timer, 1, 0), TX_SUCCESS);
    const ULONG fires_before_one_shot = threadx_example_timer_fires;
    expect(tx_timer_activate(&timer), TX_SUCCESS);
    expect(tx_thread_sleep(2), TX_SUCCESS);
    expect(threadx_example_timer_fires - fires_before_one_shot, 1);
cleanup:
    expect(tx_timer_delete(&timer), TX_SUCCESS);
    expect(tx_queue_delete(&queue), TX_SUCCESS);
    expect(tx_semaphore_delete(&semaphore), TX_SUCCESS);
    expect(tx_mutex_delete(&mutex), TX_SUCCESS);
    expect(tx_event_flags_delete(&flags), TX_SUCCESS);
    expect(tx_byte_pool_delete(&byte_pool), TX_SUCCESS);
    expect(tx_block_pool_delete(&block_pool), TX_SUCCESS);
    ++threadx_example_round;
    VA_LogString(1, "THREADX:round-complete");
}

static void controller_entry(ULONG unused)
{
    (void)unused;
    for (;;) {
        run_round();
        VA_TickOverflowCheck();
        expect(tx_thread_sleep(50), TX_SUCCESS);
    }
}

void tx_application_define(void *unused)
{
    (void)unused;
    expect(tx_semaphore_create(&command_sem, "TxCommand", 0), TX_SUCCESS);
    expect(tx_semaphore_create(&done_sem, "TxDone", 0), TX_SUCCESS);
    expect(tx_thread_create(&helper, "TxHelper", helper_entry, 0, helper_stack,
        sizeof helper_stack, 6, 6, 1, TX_AUTO_START), TX_SUCCESS);
    expect(tx_thread_create(&controller, "TxController", controller_entry, 0,
        controller_stack, sizeof controller_stack, 8, 8, TX_NO_TIME_SLICE, TX_AUTO_START), TX_SUCCESS);
}

#if defined(STM32F767xx)
int main(void)
{
    clock_init();
    VA_Init(SystemCoreClock);
    VA_RegisterUserTrace((uint8_t)(16 + EXTI0_IRQn), "TxSoftwareIRQ", VA_USER_TYPE_ISR);
    /* The host's capture path resets into this image after opening its reader. */
    VA_LogString(0, "THREADX:F767ZI:v1");
    tx_kernel_enter();
    for (;;) {}
}
#elif defined(STM32U575xx)
/* ST's unmodified U575 donor main configures its power supply, MSI PLL and
 * clocks, then calls this application boundary. No security options change. */
void MX_ThreadX_Init(void)
{
    /* HAL uses TIM6 only during initialization; ThreadX owns SysTick below. */
    NVIC_DisableIRQ(TIM6_IRQn);
#if VA_TRANSPORT == ARM_ITM
    /* U5 gates its trace power/clock separately. Firmware owns the pin mux;
     * the capture host owns the CoreSight ITM/TPIU register configuration. */
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOBEN;
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 6)) | (2u << 6);
    GPIOB->OSPEEDR |= 3u << 6;
    GPIOB->AFR[0] &= ~(15u << 12);
    DBGMCU->CR = (DBGMCU->CR & ~DBGMCU_CR_TRACE_MODE) |
                 DBGMCU_CR_TRACE_IOEN | DBGMCU_CR_TRACE_CLKEN;
#endif
    VA_Init(SystemCoreClock);
    VA_RegisterUserTrace((uint8_t)(16 + EXTI0_IRQn), "TxSoftwareIRQ", VA_USER_TYPE_ISR);
    VA_LogString(0, "THREADX:U575ZI:v1");
    tx_kernel_enter();
}
#endif
