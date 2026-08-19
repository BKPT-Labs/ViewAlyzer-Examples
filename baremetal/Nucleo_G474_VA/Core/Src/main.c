/**
 * Nucleo-G474RE — ViewAlyzer bare-metal integration example (Cortex-M4F @ 170 MHz).
 *
 * A normal-usage reference (the benchmark sibling lives in
 * throughput/Nucleo_G474_Throughput): user value traces, event spans, and
 * ISR instrumentation over the recorder core, with no RTOS.
 *
 *     -DVA_TRANSPORT=RAM_BUFFER   probe memory-read RAM ring (default) —
 *                                 works with the on-board ST-LINK, no wiring
 *     -DVA_TRANSPORT=ARM_ITM      ITM / SWO trace pin
 *
 * The workload is a layered integer DSP chain that runs every loop pass
 * (sensor -> biquad filter -> envelope -> classify -> stats), so an
 * instruction-trace snapshot always lands in real code with real call depth
 * — not in an idle delay loop. Logging is decimated: the loop runs far too
 * fast to trace every pass without flooding the ring.
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

/* Quarter-wave sine table (0..90 deg, scaled 0..100) — avoids pulling in libm */
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

/* ── Demo signal pipeline ─────────────────────────────────────────────────
 * noinline keeps the call structure visible in a flame view instead of
 * being folded away by the optimizer. */
#define DEMO_FN __attribute__((noinline))

static uint32_t lfsr_state = 0xACE1u;

/* 16-bit Galois LFSR -- cheap pseudo-noise leaf */
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
    /* b = {0.067, 0.135, 0.067}, a = {-1.143, 0.413} in Q12 */
    int32_t y = (275 * x + 550 * x1 + 275 * x2 + 4682 * y1 - 1692 * y2) >> 12;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
}

/* Attack/release envelope follower -- data-dependent branches */
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
    if (env < 800)  return 0u;            /* quiet   */
    if (env < 2400) return 1u;            /* normal  */
    if (env < 3600) return 2u;            /* loud    */
    return 3u;                            /* clipped */
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

/* ── Clock: HSI16 / 4 * 85 / 2 = 170 MHz (boost regulator, 4 WS) ── */
static void SystemClock_Config (void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    HAL_PWREx_ControlVoltageScaling (PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
    RCC_OscInitStruct.PLL.PLLN = 85;                    /* 16/4*85/2 = 170 MHz */
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig (&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig (&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
        Error_Handler();
}

/* Green LD2 on the Nucleo-G474RE (PA5) */
static void led_init (void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    (void) RCC->AHB2ENR;                              /* dummy read: let the clock settle */
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE5_Msk) | (1u << GPIO_MODER_MODE5_Pos);
}

int main (void)
{
    HAL_Init();
    SystemClock_Config();
    /* Keep the debug interface clocked in low-power modes so the probe can
     * always reach the target. */
    HAL_DBGMCU_EnableDBGSleepMode();
    HAL_DBGMCU_EnableDBGStopMode();
    HAL_DBGMCU_EnableDBGStandbyMode();
    led_init();

    /* Start the recorder. RAM_BUFFER: the host scans RAM for the control
     * block. ARM_ITM: the host configures SWO through the probe. */
    VA_Init (SystemCoreClock);

    VA_RegisterUserTrace (TRACE_SINE,     "Sine Wave",     VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_TICK,     "Tick Counter",  VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_LED,      "LED Toggle",    VA_USER_TYPE_TOGGLE);
    VA_RegisterUserTrace (TRACE_WORKLOAD, "Workload",      VA_USER_TYPE_BAR);
    VA_RegisterUserEvent (EVENT_WORK,     "Work Block");

    uint32_t step = 0;
    bool led_state = false;

    for (;;)
    {
        /* The pipeline runs EVERY iteration (no HAL_Delay: an on-chip trace
         * snapshot then always lands in real code, never in an idle poll).
         * Only every 256th pass is wrapped in the VA event -- per-pass events
         * would both flood the recorder ring and make the emit path dominate
         * the trace. */
        bool traced_pass = (step & 255u) == 0u;
        if (traced_pass) VA_EVENT_START (EVENT_WORK);
        int32_t env = process_sample (step);
        if (traced_pass) VA_EVENT_END (EVENT_WORK);

        /* VA logging is decimated -- the recorder's ring must not be flooded. */
        if ((step & 1023u) == 0u)
        {
            VA_LogTrace (TRACE_SINE, sine_lookup (step >> 10));
            VA_LogTrace (TRACE_TICK, (int32_t) (HAL_GetTick() % 1000u));
            VA_LogTrace (TRACE_WORKLOAD, env);
        }

        if ((step & 32767u) == 0u)
        {
            led_state = ! led_state;
            GPIOA->ODR ^= GPIO_ODR_OD5;               /* green LD2 */
            VA_LogToggle (TRACE_LED, led_state);
        }

        if ((step & 0x7FFFFu) == 0u)
            VA_LogString (TRACE_LOG, "Nucleo-G474 bare-metal demo alive");

        VA_TickOverflowCheck();
        ++step;
    }
}

void Error_Handler (void)
{
    __disable_irq();
    for (;;) {}
}

/* ── Exception handlers ── */
void NMI_Handler (void)        { for (;;) {} }
void HardFault_Handler (void)  { for (;;) {} }
void MemManage_Handler (void)  { for (;;) {} }
void BusFault_Handler (void)   { for (;;) {} }
void UsageFault_Handler (void) { for (;;) {} }
void SVC_Handler (void)        {}
void DebugMon_Handler (void)   {}
void PendSV_Handler (void)     {}
