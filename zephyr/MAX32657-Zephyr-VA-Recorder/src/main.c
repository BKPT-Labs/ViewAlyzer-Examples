/*
 * ViewAlyzer on the ADI MAX32657EVKIT (Cortex-M33).
 *
 * Demo app for ADI: a small but *representative* RTOS workload so the
 * ViewAlyzer timeline shows scheduling, IPC, mutex contention, and a few
 * user-value graphs — without drowning the view in traces.
 *
 * Threads
 *   main     — LED heartbeat + sine generator, publishes SINE user trace.
 *   sensor   — fake ADC (sine + pseudo-random noise), publishes SENSOR,
 *              hands each sample to `control` via a message queue.
 *   control  — consumes samples, runs a 1st-order IIR, publishes SETPOINT.
 *   worker   — periodic mutex holder, publishes WORK counter.
 *   comm     — contends on the same mutex, publishes PACKETS counter.
 *
 * Traces (5, deliberately kept small):
 *   SINE     — clean reference sine (float graph)
 *   SENSOR   — noisy "ADC" sample     (float graph)
 *   SETPOINT — filtered output        (float graph)
 *   WORK     — worker iteration count (counter)
 *   PACKETS  — comm packet count      (counter)
 *
 * Transport: SEGGER RTT over the board's on-board J-Link OB (see prj.conf).
 */

#include <math.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include "ViewAlyzer.h"
#include "VA_Adapter_Zephyr.h"

extern uint32_t SystemCoreClock;

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ---------------- User-trace IDs (small unique integers) ---------------- */
#define USER_TRACE_SINE      1
#define USER_TRACE_WORK      2
#define USER_TRACE_SENSOR    3
#define USER_TRACE_SETPOINT  4
#define USER_TRACE_PACKETS   5

#define SAMPLE_PERIOD_MS     50    /* main / sine period */
#define SENSOR_PERIOD_MS     20    /* ~50 Hz fake ADC */

/* Shared mutex — held briefly by worker and comm so their contention shows
 * up as thread switches / blocked spans on the ViewAlyzer timeline. */
static K_MUTEX_DEFINE(shared_lock);

/* Message queue: sensor -> control. Small on purpose so a slow consumer
 * would visibly back-pressure the producer in the trace. */
K_MSGQ_DEFINE(sensor_q, sizeof(float), 8, 4);

/* ---------------- Cheap deterministic PRNG (xorshift32) ----------------- */
static uint32_t rng_state = 0xC0FFEEu;
static inline float noise_pm(float amplitude)
{
    /* xorshift32 → [-amplitude, +amplitude] */
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    /* map to [-1, 1) */
    float n = ((int32_t)x) * (1.0f / 2147483648.0f);
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

/* ---------------- Sensor: fake noisy ADC -> queue ----------------------- */
static void sensor_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    while (1) {
        /* Sample the shared sine and add ±0.15 of pseudo-random noise. */
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
    const float alpha = 0.15f;            /* low-pass, ~cutoff = fs*alpha/(2π) */

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

/* ---------------- Legacy worker (kept from original demo) --------------- */
K_THREAD_DEFINE(worker_tid, 1024, worker_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(5), 0, 0);

/* ---------------- Debugger demo: deep call chain, rich locals ------------
 * Somewhere pleasant to halt. Break in pipeline_checksum() (the deepest
 * frame) and the backtrace reads:
 *   pipeline_checksum <- pipeline_scale <- pipeline_filter
 *   <- pipeline_acquire <- debug_pipeline <- main
 * Each level keeps a different KIND of local live — a struct by value, an
 * array, pointers into a caller's frame, an enum, floats, a string — so
 * every frame in the variables view shows something distinct. noinline +
 * the volatile sink keep frames and locals from being optimised away. */

typedef enum { SAMPLE_RAW = 0, SAMPLE_FILTERED, SAMPLE_SCALED, SAMPLE_DONE } sample_state_t;

typedef struct {
    const char    *name;      /* points at a string literal -> readable in the watch view */
    sample_state_t state;
    uint32_t       seq;
    int32_t        raw[4];    /* small array: expandable in the variables view */
    float          scaled;
    uint16_t       crc;
} demo_sample_t;

static volatile uint32_t pipeline_sink;   /* volatile: keep frames/locals alive at -Os */

/* level 5 (deepest) — byte-wise CRC-16 over the struct's raw[] samples.
 * Good single-step target: tight loop, several scalar locals mutating. */
static __attribute__((noinline)) uint16_t pipeline_checksum(const demo_sample_t *s)
{
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0; i < 4; i++) {
        uint16_t byte = (uint16_t)(s->raw[i] & 0xFFu);
        crc ^= (uint16_t)(byte << 8);
        for (uint32_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* level 4 — float math; locals: min/max/span plus a pointer into the struct */
static __attribute__((noinline)) float pipeline_scale(demo_sample_t *s)
{
    int32_t *first = &s->raw[0];          /* pointer into the caller-owned struct */
    int32_t  min = *first, max = *first;
    for (uint32_t i = 1; i < 4; i++) {
        if (s->raw[i] < min) { min = s->raw[i]; }
        if (s->raw[i] > max) { max = s->raw[i]; }
    }
    float span   = (float)(max - min);
    float scaled = (span > 0.0f) ? ((float)s->raw[3] - (float)min) / span : 0.0f;
    s->state  = SAMPLE_SCALED;
    s->scaled = scaled;
    s->crc    = pipeline_checksum(s);
    return scaled;
}

/* level 3 — 3-tap moving average in place; locals: a small history window */
static __attribute__((noinline)) float pipeline_filter(demo_sample_t *s)
{
    int32_t window[3] = { s->raw[0], s->raw[0], s->raw[0] };  /* warm-start history */
    for (uint32_t i = 0; i < 4; i++) {
        window[i % 3] = s->raw[i];
        s->raw[i] = (window[0] + window[1] + window[2]) / 3;
    }
    s->state = SAMPLE_FILTERED;
    return pipeline_scale(s);
}

/* level 2 — builds the sample struct ON THIS FRAME'S STACK, so every deeper
 * frame's `s` pointer resolves back into this frame while halted. */
static __attribute__((noinline)) uint32_t pipeline_acquire(uint32_t seed, uint32_t seq)
{
    demo_sample_t sample = {
        .name   = "adc-ch3",
        .state  = SAMPLE_RAW,
        .seq    = seq,
        .raw    = { 0 },
        .scaled = 0.0f,
        .crc    = 0,
    };
    uint32_t lcg = seed;
    for (uint32_t i = 0; i < 4; i++) {        /* fake ADC burst: 4 LCG samples */
        lcg = lcg * 1664525u + 1013904223u;
        sample.raw[i] = (int32_t)(lcg >> 20); /* 0..4095, ADC-ish range */
    }
    float norm = pipeline_filter(&sample);
    sample.state = SAMPLE_DONE;
    return (uint32_t)(norm * 1000.0f) ^ sample.crc;
}

/* level 1 (entry) — loop counter + accumulator locals; called from main */
static __attribute__((noinline)) uint32_t debug_pipeline(uint32_t seed)
{
    uint32_t total = 0;
    static uint32_t seq;                      /* survives calls: watchable across halts */
    for (uint32_t burst = 0; burst < 2; burst++) {
        seq++;
        total += pipeline_acquire(seed + burst, seq);
    }
    pipeline_sink = total;
    return total;
}

int main(void)
{
    if (!device_is_ready(led.port)) {
        return -1;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

    k_msleep(1000);                       /* give the debugger time to attach */

    VA_Init(SystemCoreClock);
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

        debug_pipeline(rng_state);        /* deep 5-frame chain for the debugger */

        VA_TickOverflowCheck();
        k_msleep(SAMPLE_PERIOD_MS);
    }

    return 0;
}
