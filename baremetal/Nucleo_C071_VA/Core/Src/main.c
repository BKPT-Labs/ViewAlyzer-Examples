/**
 * Nucleo-C071RB - ViewAlyzer bare-metal example with a synthetic IMU
 * (Cortex-M0+ @ 48 MHz).
 *
 * Two things run side by side:
 *
 *   1. The layered integer DSP chain from the C031 example (sensor -> biquad
 *      filter -> envelope -> classify -> stats), so PC-sample profiling always
 *      lands in real code with real call depth.
 *   2. A synthetic 6-channel IMU (imu.ax/ay/az, imu.heading, imu.temp,
 *      imu.motion) emitted as plain user traces. The `com.bkpt.demo.imu`
 *      Trace Domain descriptor in ViewAlyzer claims the `imu.*` channels and
 *      enriches them host-side: units (m/s2, deg, degC), groups, and display
 *      overrides the firmware cannot even express (heading -> angle dial,
 *      temp -> gauge). The firmware stays vendor-plain on purpose: the demo
 *      is that meaning lives in the descriptor, not in this file.
 *
 * Timestamps come from TIM3 (free-running, 1 MHz, 16-bit) through the
 * recorder's CUSTOM_TIMER source; the transport is the RAM ring the on-board
 * ST-LINK drains with non-intrusive memory reads. Logging is decimated: the
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
#define TRACE_IMU_AX      50
#define TRACE_IMU_AY      51
#define TRACE_IMU_AZ      52
#define TRACE_IMU_HEAD    53
#define TRACE_IMU_TEMP    54
#define TRACE_IMU_MOT     55
#define TRACE_JSYNC       60
#define TRACE_PSTATE      61
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

/* ── Synthetic IMU ────────────────────────────────────────────────────
 * A slowly yawing, gently tilting body with occasional shake bursts.
 * sine_lookup() is the only trig (64 steps per period, -100..100), scaled
 * to physical units in float at log time; the "motion" bursts ride the
 * LFSR so the pattern never repeats exactly. */
typedef struct
{
    float ax, ay, az;      /* m/s2 */
    float heading;         /* deg, 0..360 */
    float temp;            /* degC */
    bool  moving;
} ImuSample;

static uint32_t imu_phase;         /* advances once per IMU sample */
static uint32_t imu_heading_x10;   /* 0..3599, tenths of a degree */
static uint32_t imu_burst;         /* samples of shake left */

DEMO_FN static void imu_sample (ImuSample *out)
{
    ++imu_phase;

    /* Yaw: base rate ~0.6 deg/sample, modulated by a very slow sine. */
    uint32_t rate_x10 = 6u + (uint32_t) ((3 * sine_lookup (imu_phase >> 6) + 300) / 100);
    imu_heading_x10 = (imu_heading_x10 + rate_x10) % 3600u;

    /* Shake bursts: start on a rare LFSR pattern, last 24 samples. */
    if (imu_burst == 0u && (lfsr_state & 0x3Fu) == 0x2Au)
        imu_burst = 24u;
    bool moving = imu_burst > 0u;
    if (moving)
        --imu_burst;

    /* Tilt: +-12 deg roll and pitch on slow, incommensurate periods; the
     * gravity vector projects onto the axes. Small-angle scale: at 12 deg,
     * sin ~ 0.20, so 100-scale sine * 0.02 m/s2 fits. */
    int32_t roll_s  = sine_lookup (imu_phase >> 3);            /* -100..100 */
    int32_t pitch_s = sine_lookup ((imu_phase >> 2) + 17u);
    float vib = moving ? (float) noise_lfsr() * 0.02f : 0.0f;  /* +-2.5 m/s2 */
    out->ax = (float) roll_s * 0.020f + vib;
    out->ay = (float) pitch_s * 0.020f + (moving ? (float) noise_lfsr() * 0.015f : 0.0f);
    out->az = 9.81f - (float) (roll_s * roll_s + pitch_s * pitch_s) * 0.0001f
            + (moving ? (float) noise_lfsr() * 0.01f : 0.0f);

    out->heading = (float) imu_heading_x10 * 0.1f;
    /* Temperature: 25 C + slow drift + self-heating while moving. */
    out->temp = 25.0f + (float) sine_lookup (imu_phase >> 9) * 0.015f + (moving ? 0.4f : 0.0f);
    out->moving = moving;
}

/* ── External-instrument sync marks ───────────────────────────────────
 * Any bench instrument that can timestamp a logic edge on its own clock
 * (power analyzer GPI, logic analyzer, DAQ, scope) can be merged onto this
 * recording's time axis: wire PC10 (morpho CN7 pin 1) + GND to the
 * instrument, record the edge times, and hand them to
 * `viewalyzer-cli import` as the sync train (NDJSON contract).
 *
 * Each mark is a rising edge plus a VA_LogTrace("jsync", seq) event inside
 * one interrupt-masked section, so edge-to-timestamp skew is a constant few
 * instructions (the import's linear fit absorbs it). Delays are
 * LFSR-dithered (400..700 ms in 20 ms steps) so the interval sequence is a
 * unique fingerprint: alignment is unambiguous no matter when the
 * instrument capture started. A fixed period would be ambiguous modulo one
 * period. */
#define SYNC_GPIO_PORT  GPIOC
#define SYNC_GPIO_PIN   GPIO_PIN_10

static void sync_gpio_init (void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    HAL_GPIO_WritePin (SYNC_GPIO_PORT, SYNC_GPIO_PIN, GPIO_PIN_RESET);
    g.Pin   = SYNC_GPIO_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init (SYNC_GPIO_PORT, &g);
}

static void sync_mark (uint32_t seq)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    SYNC_GPIO_PORT->BSRR = SYNC_GPIO_PIN;                 /* rising edge      */
    VA_LogTrace (TRACE_JSYNC, (int32_t) seq);             /* timestamped mark */
    SYNC_GPIO_PORT->BSRR = (uint32_t) SYNC_GPIO_PIN << 16;
    __set_PRIMASK (primask);
}

/* Galois LFSR (seed 0xACE1, taps 0xB400): the import's synthetic test
 * mirrors this generator. Separate state from noise_lfsr on purpose. */
static uint32_t sync_next_delay_ms (void)
{
    static uint16_t lfsr = 0xACE1u;
    lfsr = (uint16_t) ((lfsr >> 1) ^ ((lfsr & 1u) ? 0xB400u : 0u));
    return 400u + (uint32_t) (lfsr & 0xFu) * 20u;
}

/* ── Power-profile stages ─────────────────────────────────────────────
 * A repeating 4-stage cycle (600 ms each) that gives an external power
 * instrument distinct signatures to see, each announced on the pwr.stage
 * trace so the current curve correlates with a cause:
 *   0 busy    the normal DSP pipeline (baseline)
 *   1 sleep   WFI between SysTicks: core clock-gated, the big dip
 *   2 led     user LED solid on: measures whether the LED sits behind
 *             the IDD measurement point at all
 *   3 clocks  a bank of peripheral clocks enabled (clock-tree power)
 * Sync marks keep firing on schedule in every stage (the sleep loop wakes
 * at 1 kHz), so instrument alignment is unaffected. */
static void pwr_clocks (bool on)
{
    if (on)
    {
        __HAL_RCC_TIM1_CLK_ENABLE();
        __HAL_RCC_TIM14_CLK_ENABLE();
        __HAL_RCC_TIM16_CLK_ENABLE();
        __HAL_RCC_TIM17_CLK_ENABLE();
        __HAL_RCC_USART1_CLK_ENABLE();
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_SPI1_CLK_ENABLE();
        __HAL_RCC_I2C1_CLK_ENABLE();
        __HAL_RCC_ADC_CLK_ENABLE();
    }
    else
    {
        __HAL_RCC_TIM1_CLK_DISABLE();
        __HAL_RCC_TIM14_CLK_DISABLE();
        __HAL_RCC_TIM16_CLK_DISABLE();
        __HAL_RCC_TIM17_CLK_DISABLE();
        __HAL_RCC_USART1_CLK_DISABLE();
        __HAL_RCC_USART2_CLK_DISABLE();
        __HAL_RCC_SPI1_CLK_DISABLE();
        __HAL_RCC_I2C1_CLK_DISABLE();
        __HAL_RCC_ADC_CLK_DISABLE();
    }
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

/* Green LD1 on the Nucleo-C071RB (PA5, same as every Nucleo-64) */
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
    sync_gpio_init();
    tick_timer_init();

    /* Start the recorder: RAM ring in SRAM (the host scans for the control
     * block), timestamps from TIM3. */
    VA_Init (SystemCoreClock, tick_read, TICK_HZ);

    VA_RegisterUserTrace (TRACE_SINE,     "Sine Wave",     VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_TICK,     "Tick Counter",  VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_LED,      "LED Toggle",    VA_USER_TYPE_TOGGLE);
    VA_RegisterUserTrace (TRACE_WORKLOAD, "Workload",      VA_USER_TYPE_BAR);
    VA_RegisterUserEvent (EVENT_WORK,     "Work Block");

    /* The IMU channels register with plain firmware hints (GRAPH / TOGGLE).
     * The com.bkpt.demo.imu descriptor upgrades them host-side. */
    VA_RegisterUserTrace (TRACE_IMU_AX,   "imu.ax",        VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_IMU_AY,   "imu.ay",        VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_IMU_AZ,   "imu.az",        VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_IMU_HEAD, "imu.heading",   VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_IMU_TEMP, "imu.temp",      VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_IMU_MOT,  "imu.motion",    VA_USER_TYPE_TOGGLE);
    VA_RegisterUserTrace (TRACE_JSYNC,    "jsync",         VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_PSTATE,   "pwr.stage",     VA_USER_TYPE_GRAPH);

    uint32_t step = 0;
    bool was_moving = false;
    uint32_t sync_seq = 0;
    uint32_t sync_next_ms = 0;   /* startup burst at ~0/50/100 ms, then dithered */
    uint32_t pwr_stage = 0;
    uint32_t pwr_next_ms = 600;
    VA_LogTrace (TRACE_PSTATE, 0);

    for (;;)
    {
        /* Power-profile stage machine (fixed 600 ms cadence). */
        if ((int32_t) (HAL_GetTick() - pwr_next_ms) >= 0)
        {
            pwr_stage = (pwr_stage + 1u) & 3u;
            pwr_next_ms += 600u;
            VA_LogTrace (TRACE_PSTATE, (int32_t) pwr_stage);
            bool led_on = pwr_stage == 2u;
            HAL_GPIO_WritePin (GPIOA, GPIO_PIN_5, led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
            VA_LogToggle (TRACE_LED, led_on);
            pwr_clocks (pwr_stage == 3u);
        }

        if (pwr_stage == 1u)
        {
            /* Sleep stage: WFI until the next SysTick; the DSP pipeline
             * pauses (that is the point), sync marks stay on time below. */
            __WFI();
        }
        else
        {
            /* The pipeline runs EVERY iteration (no HAL_Delay: a PC sample
             * then always lands in real code, never in an idle poll). Only
             * every 256th pass is wrapped in the VA event: per-pass events
             * would flood the ring. */
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

            /* IMU: one sample every 1024th pass; motion is edge-logged only. */
            if ((step & 1023u) == 0u)
            {
                ImuSample s;
                imu_sample (&s);
                VA_LogTraceFloat (TRACE_IMU_AX, s.ax);
                VA_LogTraceFloat (TRACE_IMU_AY, s.ay);
                VA_LogTraceFloat (TRACE_IMU_AZ, s.az);
                VA_LogTraceFloat (TRACE_IMU_HEAD, s.heading);
                if ((step & 8191u) == 0u)
                    VA_LogTraceFloat (TRACE_IMU_TEMP, s.temp);
                if (s.moving != was_moving)
                {
                    was_moving = s.moving;
                    VA_LogToggle (TRACE_IMU_MOT, s.moving);
                }
            }

            if ((step & 0x7FFFFu) == 0u)
                VA_LogString (TRACE_LOG, "Nucleo-C071 IMU demo alive");

            ++step;
        }

        /* Instrument sync marks, paced by the HAL tick: runs in EVERY stage
         * (the sleep loop wakes at 1 kHz), so a mark is never late and the
         * interval fingerprint stays intact. */
        if ((int32_t) (HAL_GetTick() - sync_next_ms) >= 0)
        {
            sync_mark (sync_seq);
            sync_next_ms = HAL_GetTick() + (sync_seq < 2u ? 50u : sync_next_delay_ms());
            ++sync_seq;
        }

        VA_TickOverflowCheck();               /* 16-bit tick: every pass */
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
