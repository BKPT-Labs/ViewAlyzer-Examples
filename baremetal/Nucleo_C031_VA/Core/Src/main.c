/**
 * Nucleo-C031C6 - ViewAlyzer bare-metal integration example (Cortex-M0+ @ 48 MHz).
 *
 * The smallest-core reference: no DWT cycle counter, no ITM, 12 KB of SRAM.
 * Timestamps come from TIM3 (free-running, 1 MHz, 16-bit) through the
 * recorder's CUSTOM_TIMER source; the transport is the RAM ring the on-board
 * ST-LINK drains with non-intrusive memory reads. DWT_PCSR is implemented on
 * this core, so the ViewAlyzer app can also sample the program counter by
 * debug-port polling for a live statistical profile.
 *
 * The workload is a layered integer DSP chain that runs every loop pass
 * (sensor -> biquad filter -> envelope -> classify -> stats), so a PC sample
 * always lands in real code with real call depth. Logging is decimated: the
 * loop runs far too fast to trace every pass without flooding the ring.
 */
#include "main.h"
#include "ViewAlyzer.h"

/* Trace / event IDs */
#define TRACE_SINE        42
#define TRACE_TICK        43
#define TRACE_LED         44
#define EVENT_WORK        45
#define TRACE_WORKLOAD    46
#define TRACE_LOG          1

/* Quarter-wave sine table (0..90 deg, scaled 0..100) - avoids pulling in libm */
static const int16_t sine_quarter[16] = {
    0, 10, 20, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 100
};

static int16_t sine_lookup (uint32_t idx)
{
    idx &= 63u;                       /* 64 steps per period */
    uint32_t quadrant = idx >> 4;     /* 0..3 */
    uint32_t i = idx & 15u;
    switch (quadrant)
    {
        case 0:  return sine_quarter[i];
        case 1:  return sine_quarter[15u - i];
        case 2:  return (int16_t) -sine_quarter[i];
        default: return (int16_t) -sine_quarter[15u - i];
    }
}

/* noinline keeps the call structure visible in a profile instead of being
 * folded away by the optimizer. */
#define DEMO_FN __attribute__((noinline))

static uint32_t lfsr_state = 0xACE1u;

/* 16-bit Galois LFSR: cheap pseudo-noise leaf */
DEMO_FN static int32_t noise_lfsr (void)
{
    uint32_t l = lfsr_state;
    l = (l >> 1) ^ (uint32_t) (-(int32_t) (l & 1u) & 0xB400u);
    lfsr_state = l;
    return (int32_t) (l & 0xFFu) - 128;
}

/* Synthesized "sensor": sine carrier + noise, occasional glitch spike */
DEMO_FN static int32_t sensor_read (uint32_t step)
{
    int32_t s = 40 * sine_lookup (step) + noise_lfsr();
    if ((step & 1023u) == 511u)
        s += 3000;                        /* rare glitch: exercises the branchy paths */
    return s;
}

/* Integer biquad low-pass (fixed Q12 coefficients, static state) */
DEMO_FN static int32_t filter_biquad (int32_t x)
{
    static int32_t x1, x2, y1, y2;
    int32_t y = (275 * x + 550 * x1 + 275 * x2 + 4682 * y1 - 1692 * y2) >> 12;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
}

/* Attack/release envelope follower: data-dependent branches */
DEMO_FN static int32_t envelope_track (int32_t v)
{
    static int32_t env;
    int32_t mag = v < 0 ? -v : v;
    if (mag > env)
        env += (mag - env) >> 2;          /* fast attack  */
    else
        env -= (env - mag) >> 6;          /* slow release */
    return env;
}

/* Classify the envelope into one of four bands */
DEMO_FN static uint32_t classify (int32_t env)
{
    if (env < 800)  return 0u;
    if (env < 2400) return 1u;
    if (env < 3600) return 2u;
    return 3u;
}

/* Loopy integer square root leaf (no libm) */
DEMO_FN static uint32_t isqrt32 (uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit)
    {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else               r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Running stats + band histogram; RMS via isqrt32 every 64 samples */
static uint32_t band_hist[4];
static int64_t  sq_acc;
static uint32_t rms;

DEMO_FN static void stats_update (uint32_t band, int32_t v, uint32_t step)
{
    band_hist[band]++;
    sq_acc += (int64_t) v * v;
    if ((step & 63u) == 63u)
    {
        rms = isqrt32 ((uint32_t) (sq_acc >> 6));
        sq_acc = 0;
    }
}

/* One pipeline pass: the whole chain, top of the demo call tree */
DEMO_FN static int32_t process_sample (uint32_t step)
{
    int32_t raw      = sensor_read (step);
    int32_t filtered = filter_biquad (raw);
    int32_t env      = envelope_track (filtered);
    uint32_t band    = classify (env);
    stats_update (band, filtered, step);
    return env;
}

/* ── SysTick: HAL 1 kHz tick + a traced ISR band on the timeline ── */
void SysTick_Handler (void)
{
    VA_LogISRStart (VA_ISR_ID_SYSTICK);
    HAL_IncTick();
    VA_LogISREnd (VA_ISR_ID_SYSTICK);
}

/* ── Clock: HSI 48 MHz, no divider (1 WS) ── */
static void SystemClock_Config (void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    if (HAL_RCC_OscConfig (&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig (&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
        Error_Handler();
}

/* ── Timestamp source: TIM3 free-running at 1 MHz (16-bit) ────────────
 * The recorder reads CNT inside every hook; it must count before VA_Init()
 * and never stop. 16 bits at 1 MHz wrap every 65 ms, so the main loop calls
 * VA_TickOverflowCheck() every pass (far more often than that). */
#define TICK_HZ 1000000u

static void tick_timer_init (void)
{
    __HAL_RCC_TIM3_CLK_ENABLE();
    TIM3->PSC = (uint16_t) (SystemCoreClock / TICK_HZ - 1u);
    TIM3->ARR = 0xFFFFu;
    TIM3->EGR = TIM_EGR_UG;                       /* latch PSC */
    TIM3->CR1 = TIM_CR1_CEN;
}

static uint32_t tick_read (void)
{
    return TIM3->CNT;
}

/* Green LD4 on the Nucleo-C031C6 (PA5) */
static void led_init (void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin   = GPIO_PIN_5;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init (GPIOA, &g);
}

int main (void)
{
    HAL_Init();
    SystemClock_Config();
    /* Keep the debug interface clocked in low-power modes so the probe can
     * always reach the target. */
    HAL_DBGMCU_EnableDBGStopMode();
    HAL_DBGMCU_EnableDBGStandbyMode();
    led_init();
    tick_timer_init();

    /* Start the recorder: RAM ring in SRAM (the host scans for the control
     * block), timestamps from TIM3. */
    VA_Init (SystemCoreClock, tick_read, TICK_HZ);

    VA_RegisterUserTrace (TRACE_SINE,     "Sine Wave",     VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_TICK,     "Tick Counter",  VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_LED,      "LED Toggle",    VA_USER_TYPE_TOGGLE);
    VA_RegisterUserTrace (TRACE_WORKLOAD, "Workload",      VA_USER_TYPE_BAR);
    VA_RegisterUserEvent (EVENT_WORK,     "Work Block");

    uint32_t step = 0;
    bool led_state = false;

    for (;;)
    {
        /* The pipeline runs EVERY iteration (no HAL_Delay: a PC sample then
         * always lands in real code, never in an idle poll). Only every 256th
         * pass is wrapped in the VA event: per-pass events would flood the
         * 4 KB ring. */
        bool traced_pass = (step & 255u) == 0u;
        if (traced_pass) VA_EVENT_START (EVENT_WORK);
        int32_t env = process_sample (step);
        if (traced_pass) VA_EVENT_END (EVENT_WORK);

        /* VA logging is decimated: the ring must not be flooded. */
        if ((step & 2047u) == 0u)
        {
            VA_LogTrace (TRACE_SINE, sine_lookup (step >> 11));
            VA_LogTrace (TRACE_TICK, (int32_t) (HAL_GetTick() % 1000u));
            VA_LogTrace (TRACE_WORKLOAD, env);
        }

        if ((step & 32767u) == 0u)
        {
            led_state = ! led_state;
            HAL_GPIO_TogglePin (GPIOA, GPIO_PIN_5);
            VA_LogToggle (TRACE_LED, led_state);
        }

        if ((step & 0x7FFFFu) == 0u)
            VA_LogString (TRACE_LOG, "Nucleo-C031 bare-metal demo alive");

        VA_TickOverflowCheck();               /* 16-bit tick: every pass */
        ++step;
    }
}

void Error_Handler (void)
{
    __disable_irq();
    for (;;) {}
}

/* ── Exception handlers (ARMv6-M has no fault handlers beyond HardFault) ── */
void NMI_Handler (void)        { for (;;) {} }
void HardFault_Handler (void)  { for (;;) {} }
void SVC_Handler (void)        {}
void PendSV_Handler (void)     {}
