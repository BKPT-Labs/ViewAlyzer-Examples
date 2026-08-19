/*
 * ViewAlyzer on the ST NUCLEO-L031K6 (STM32L031K6, Cortex-M0+).
 *
 * The 8 KB RAM floor test: the L031 has no DWT, no ITM, and no
 * 32-bit timer, so timestamps come from 16-bit TIM2 through the recorder's
 * CUSTOM_TIMER source with VA_TIMER_BITS=16, and the transport is the RAM
 * buffer drained by the on-board ST-LINK.
 *
 * Kept deliberately tiny (8 KB RAM / 32 KB flash): a heartbeat in main
 * plus one worker thread contending on a mutex, publishing two integer
 * counters - enough for the timeline to show scheduling, and nothing that
 * needs a float libc.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>                     /* TIM2 / RCC register definitions */

#include "ViewAlyzer.h"
#include "VA_Adapter_Zephyr.h"

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ---------------- Timestamp source: TIM2 as a 100 kHz free-runner -------
 * TIM2 is 16-bit, so the wrap budget rules from ViewAlyzerConfig.h apply:
 * at 100 kHz the counter wraps every 655 ms, and the recorder must see at
 * least one clock read per wrap. Every emitted event reads the clock, and
 * the main loop below calls VA_TickOverflowCheck() every 50 ms as the
 * quiet-gap backstop - a >13x margin. Timestamp resolution is 10 us.
 *
 * The timer kernel clock equals the CPU clock (32 MHz) because this board
 * config runs AHB and APB undivided (Zephyr's nucleo_l031k6 devicetree);
 * with an APB prescaler other than /1, STM32 timers clock at 2x APB and
 * this prescaler computation would need that factor. Nothing else in this
 * project claims TIM2 (no PWM/counter driver is enabled).
 */
#define TICK_HZ           100000u
#define TIM2_CLOCK_HZ     ((uint32_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC)

static void tick_timer_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;              /* read back: peripheral-enable delay */

    TIM2->PSC = (uint16_t)((TIM2_CLOCK_HZ / TICK_HZ) - 1u);
    TIM2->ARR = 0xFFFFu;             /* full 16-bit span */
    TIM2->EGR = TIM_EGR_UG;          /* latch PSC/ARR now, not at wrap */
    TIM2->CR1 = TIM_CR1_CEN;
}

static uint32_t tick_timer_read(void)
{
    return TIM2->CNT;                /* low 16 bits used (VA_TIMER_BITS=16) */
}

/* ---------------- User-trace IDs ---------------------------------------- */
#define USER_TRACE_BEAT   1
#define USER_TRACE_WORK   2

#define BEAT_PERIOD_MS    50

static K_MUTEX_DEFINE(shared_lock);

/* ---------------- Worker: periodic mutex holder ------------------------- */
static void worker_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    int32_t work = 0;

    while (1) {
        k_mutex_lock(&shared_lock, K_FOREVER);
        work += 1;
        VA_LogTrace(USER_TRACE_WORK, work);
        k_busy_wait(300);                 /* 0.3 ms of "work" under the lock */
        k_mutex_unlock(&shared_lock);

        k_msleep(120);
    }
}
K_THREAD_DEFINE(worker_tid, 640, worker_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(5), 0, 0);

int main(void)
{
    if (!device_is_ready(led.port)) {
        return -1;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

    k_msleep(1000);                       /* give the host time to attach */

    /* The tick source must be RUNNING before VA_Init: it probes the timer
     * and reports ERR:TS_DEAD to the host (and refuses to start) if the
     * counter is not moving. To see that failure path in the app or CLI,
     * comment out this call. */
    tick_timer_init();

    VA_Init((uint32_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
            tick_timer_read, TICK_HZ);
    VA_Zephyr_RegisterExistingThreads();

    VA_RegisterUserTrace(USER_TRACE_BEAT, "Beat", VA_USER_TYPE_COUNTER);
    VA_RegisterUserTrace(USER_TRACE_WORK, "Work", VA_USER_TYPE_COUNTER);

    int32_t beat = 0;
    while (1) {
        gpio_pin_toggle_dt(&led);

        k_mutex_lock(&shared_lock, K_FOREVER);
        VA_LogTrace(USER_TRACE_BEAT, ++beat);
        k_mutex_unlock(&shared_lock);

        /* 16-bit source: this is the wrap-budget backstop (see above). */
        VA_TickOverflowCheck();
        k_msleep(BEAT_PERIOD_MS);
    }

    return 0;
}
