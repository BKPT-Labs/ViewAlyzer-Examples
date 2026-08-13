/*
 * ViewAlyzer on the ST NUCLEO-G0B1RE (STM32G0B1RE, Cortex-M0+).
 *
 * The point of this example: a core with NO DWT cycle counter and NO ITM
 * still gets full ViewAlyzer tracing. Timestamps come from TIM2 (the G0's
 * 32-bit general-purpose timer) through the CUSTOM_TIMER source, and the
 * transport is the RAM buffer drained by the on-board ST-LINK.
 *
 * Threads (a small but representative RTOS workload):
 *   main     - LED heartbeat + sine generator, publishes SINE user trace.
 *   sensor   - fake ADC (sine + pseudo-random noise), publishes SENSOR,
 *              hands each sample to `control` via a message queue.
 *   control  - consumes samples, runs a 1st-order IIR, publishes SETPOINT.
 *   worker   - periodic mutex holder, publishes WORK counter.
 *   comm     - contends on the same mutex, publishes PACKETS counter.
 */

#include <math.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>                     /* TIM2 / RCC register definitions */

#include "ViewAlyzer.h"
#include "VA_Adapter_Zephyr.h"

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ---------------- Timestamp source: TIM2 as a 1 MHz free-runner ---------
 * TIM2 is 32-bit on the G0 family, so at 1 MHz it wraps every ~71 minutes
 * and the default VA_TickOverflowCheck() cadence is more than enough.
 * 1 MHz (instead of the full timer clock) also demonstrates that the tick
 * rate is independent of the CPU clock: the host converts with the rate
 * VA_Init() reports, so timestamps stay correct at any prescale.
 *
 * The timer kernel clock equals the CPU clock here because this board
 * config runs APB1 undivided (Zephyr's nucleo_g0b1re devicetree); with an
 * APB prescaler other than /1, STM32 timers clock at 2x APB and this
 * prescaler computation would need that factor. Nothing else in this
 * project claims TIM2 (no PWM/counter driver is enabled).
 */
#define TICK_HZ           1000000u
#define TIM2_CLOCK_HZ     ((uint32_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC)

static void tick_timer_init(void)
{
    RCC->APBENR1 |= RCC_APBENR1_TIM2EN;
    (void)RCC->APBENR1;              /* read back: peripheral-enable delay */

    TIM2->PSC = (TIM2_CLOCK_HZ / TICK_HZ) - 1u;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->EGR = TIM_EGR_UG;          /* latch PSC/ARR now, not at wrap */
    TIM2->CR1 = TIM_CR1_CEN;
}

static uint32_t tick_timer_read(void)
{
    return TIM2->CNT;
}

/* ---------------- User-trace IDs (small unique integers) ---------------- */
#define USER_TRACE_SINE      1
#define USER_TRACE_WORK      2
#define USER_TRACE_SENSOR    3
#define USER_TRACE_SETPOINT  4
#define USER_TRACE_PACKETS   5

#define SAMPLE_PERIOD_MS     50    /* main / sine period */
#define SENSOR_PERIOD_MS     20    /* ~50 Hz fake ADC */

/* Shared mutex - held briefly by worker and comm so their contention shows
 * up as thread switches / blocked spans on the ViewAlyzer timeline. */
static K_MUTEX_DEFINE(shared_lock);

/* Message queue: sensor -> control. Small on purpose so a slow consumer
 * would visibly back-pressure the producer in the trace. */
K_MSGQ_DEFINE(sensor_q, sizeof(float), 8, 4);

/* ---------------- Cheap deterministic PRNG (xorshift32) ----------------- */
static uint32_t rng_state = 0xC0FFEEu;
static inline float noise_pm(float amplitude)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    float n = ((int32_t)x) * (1.0f / 2147483648.0f);   /* [-1, 1) */
    return n * amplitude;
}

/* ---------------- Sine generator (shared reference signal) -------------- */
static volatile float sinval;

static float simple_sine_wave(void)
{
    static float angle;
    float value = sinf(angle);
    angle += 0.0628f;                     /* 2*pi / 100 */
    if (angle > 6.2832f) {
        angle -= 6.2832f;
    }
    sinval = value;
    return value;
}

/* ---------------- Worker: periodic mutex holder ------------------------- */
static void worker_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    int32_t work = 0;

    while (1) {
        k_mutex_lock(&shared_lock, K_FOREVER);
        work += 1;
        VA_LogTrace(USER_TRACE_WORK, work);
        k_busy_wait(500);                 /* 0.5 ms of "work" under the lock */
        k_mutex_unlock(&shared_lock);

        k_msleep(200);
    }
}
K_THREAD_DEFINE(worker_tid, 1024, worker_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(5), 0, 0);

/* ---------------- Sensor: fake noisy ADC -> queue ----------------------- */
static void sensor_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    while (1) {
        float sample = sinval + noise_pm(0.15f);
        VA_LogTraceFloat(USER_TRACE_SENSOR, sample);

        /* Non-blocking send; if the consumer is late we drop, keeping the
         * producer's cadence honest on the timeline. */
        (void)k_msgq_put(&sensor_q, &sample, K_NO_WAIT);

        k_msleep(SENSOR_PERIOD_MS);
    }
}
K_THREAD_DEFINE(sensor_tid, 1024, sensor_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(6), 0, 0);

/* ---------------- Control: IIR filter on sensor stream ------------------ */
static void control_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    float y = 0.0f;
    const float alpha = 0.15f;

    while (1) {
        float x;
        if (k_msgq_get(&sensor_q, &x, K_FOREVER) == 0) {
            y = y + alpha * (x - y);
            VA_LogTraceFloat(USER_TRACE_SETPOINT, y);
        }
    }
}
K_THREAD_DEFINE(control_tid, 1024, control_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(4), 0, 0);

/* ---------------- Comm: contends on the shared mutex -------------------- */
static void comm_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    uint32_t packets = 0;

    while (1) {
        k_mutex_lock(&shared_lock, K_FOREVER);
        packets++;
        VA_LogTrace(USER_TRACE_PACKETS, packets);
        k_busy_wait(200);                 /* brief hold, unlike worker */
        k_mutex_unlock(&shared_lock);

        k_msleep(150);                    /* offset from worker's 200 ms */
    }
}
K_THREAD_DEFINE(comm_tid, 1024, comm_thread, NULL, NULL, NULL,
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

    VA_RegisterUserTrace(USER_TRACE_SINE,     "Sine",     VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(USER_TRACE_SENSOR,   "Sensor",   VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(USER_TRACE_SETPOINT, "Setpoint", VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(USER_TRACE_WORK,     "Work",     VA_USER_TYPE_COUNTER);
    VA_RegisterUserTrace(USER_TRACE_PACKETS,  "Packets",  VA_USER_TYPE_COUNTER);

    while (1) {
        gpio_pin_toggle_dt(&led);

        k_mutex_lock(&shared_lock, K_FOREVER);
        VA_LogTraceFloat(USER_TRACE_SINE, simple_sine_wave());
        k_mutex_unlock(&shared_lock);

        VA_TickOverflowCheck();
        k_msleep(SAMPLE_PERIOD_MS);
    }

    return 0;
}
