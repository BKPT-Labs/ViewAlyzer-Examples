/**
 * Nucleo-F767ZI — ViewAlyzer transport throughput benchmark (bare-metal, CMSIS-only).
 *
 * J-Link-focused sibling of the Nucleo_H723_Throughput project: same paced
 * saturation loop, same _VA_TP offered/dropped accounting (-DVA_TP_TEST=1),
 * but built without any HAL/BSP — pure CMSIS register init — and with the
 * transport switch aimed at J-Link use cases:
 *
 *     -DVA_TRANSPORT=RAM_BUFFER   probe-agnostic RAM ring (default)
 *     -DVA_TRANSPORT=JLINK_RTT    SEGGER RTT (vendored SEGGER_RTT sources)
 *
 * Offered load: -DTP_TARGET_KBPS=<n> KiB/s (0 = unbounded blast). The default
 * saturates every debug-probe transport so the app's delivered KB/s reading
 * is the transport ceiling.
 *
 * Clocks: HSI16 -> PLL -> 96 MHz sysclk (VOS scale-3 territory, no overdrive,
 * 3 flash wait states). Caches stay off; all RAM in DTCM (never cached, and
 * inside the ViewAlyzer app's default control-block scan window).
 */
#include "main.h"
#include "ViewAlyzer.h"

#ifndef TP_TARGET_KBPS
#define TP_TARGET_KBPS 8000u          /* offered-load ceiling in KiB/s (0 = unbounded) */
#endif

/* Trace IDs */
#define TRACE_TP     50               /* throughput payload graph            */
#define TRACE_HB     51               /* 1 Hz heartbeat toggle               */
#define TRACE_LOG     1

static volatile uint32_t g_ms;        /* SysTick millisecond counter */

/* ── Live-watch speed demo ────────────────────────────────────────────────
 * Globals for the IDE's LIVE WATCH (raw memory polling through the debug
 * port while the core runs — no trace transport involved). The BKPT live
 * channel samples a 4-byte variable at ~2.4 kHz; these move fast enough to
 * show it: a 50 Hz sine a slow poller renders as noise, a 5 Hz fairness
 * baseline, a 1→60 Hz chirp that visually marks any viewer's Nyquist limit,
 * and a raw loop counter. */
/* Linearly interpolated sine: phase in 1/4096ths of a cycle. The bare 64-entry
 * table quantizes into visible stairs once the viewer samples faster than the
 * table steps -- interpolation keeps the demo waveform smooth at any rate. */
static int16_t sine_lookup(uint32_t idx);          /* table impl below */
static int32_t sine_smooth(uint32_t phase_4096)
{
    uint32_t idx  = (phase_4096 >> 6) & 63u;          /* table entry            */
    uint32_t frac = phase_4096 & 63u;                 /* 1/64th between entries */
    int32_t a = sine_lookup(idx);
    int32_t b = sine_lookup(idx + 1u);
    return a + (((b - a) * (int32_t) frac) >> 6);
}

volatile int32_t  g_lw_sine_50hz;
volatile int32_t  g_lw_sine_5hz;
volatile int32_t  g_lw_chirp;
volatile uint32_t g_lw_loops;

/* Quarter-wave sine (0..90 deg, 0..1000) — recognizable payload, no libm. */
static const int16_t sine_quarter[16] = {
    0, 98, 195, 290, 383, 471, 556, 634, 707, 773, 831, 881, 924, 957, 981, 995
};

static int16_t sine_lookup (uint32_t idx)
{
    idx &= 63u;
    uint32_t quadrant = idx >> 4;
    uint32_t i = idx & 15u;
    switch (quadrant)
    {
        case 0:  return sine_quarter[i];
        case 1:  return sine_quarter[15u - i];
        case 2:  return (int16_t) -sine_quarter[i];
        default: return (int16_t) -sine_quarter[15u - i];
    }
}

/* ── SysTick: 1 kHz tick + a traced ISR band on the timeline ── */
void SysTick_Handler (void)
{
    VA_LogISRStart (VA_ISR_ID_SYSTICK);
    ++g_ms;
    VA_LogISREnd (VA_ISR_ID_SYSTICK);
}

/* ── Clock: HSI16 / M16 * N192 / P2 = 96 MHz ─────────────────────────────── */
static void clock_config_96mhz (void)
{
    /* 3 wait states for 96 MHz @ 2.7-3.6 V, keep ART accelerator + prefetch off
     * (deterministic simple setup; benchmark is transport-bound anyway). */
    FLASH->ACR = FLASH_ACR_LATENCY_3WS;

    RCC->PLLCFGR = (16u << RCC_PLLCFGR_PLLM_Pos)      /* HSI16 / 16 = 1 MHz  */
                 | (192u << RCC_PLLCFGR_PLLN_Pos)     /* VCO 192 MHz         */
                 | (0u << RCC_PLLCFGR_PLLP_Pos)       /* /2 -> 96 MHz        */
                 | (4u << RCC_PLLCFGR_PLLQ_Pos);      /* 48 MHz (unused)     */
                                                      /* PLLSRC = HSI (bit clear) */
    RCC->CR |= RCC_CR_PLLON;
    while (! (RCC->CR & RCC_CR_PLLRDY)) {}

    /* AHB /1 (96), APB1 /2 (48 <= 54), APB2 /1 (96 <= 108) */
    RCC->CFGR = RCC_CFGR_PPRE1_DIV2;

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}

    SystemCoreClockUpdate();                          /* -> 96 MHz */
}

/* Green LD1 on the Nucleo-F767ZI (PB0) — a visible heartbeat so a flashed
 * board doesn't look dead (the benchmark has no HAL/BSP; everything else it
 * does is only visible through the trace stream). */
static void led_init (void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    (void) RCC->AHB1ENR;                              /* dummy read: let the clock settle */
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER0_Msk) | (1u << GPIO_MODER_MODER0_Pos);
}

int main (void)
{
    clock_config_96mhz();
    SysTick_Config (SystemCoreClock / 1000u);         /* 1 kHz tick */
    led_init();

    /* Start the recorder. RAM_BUFFER: control block lands in DTCM and the
     * host scans for it. JLINK_RTT: recorder configures its own RTT channel. */
    VA_Init (SystemCoreClock);

    VA_RegisterUserTrace (TRACE_TP, "Throughput Pay", VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace (TRACE_HB, "Heartbeat",          VA_USER_TYPE_TOGGLE);

    uint32_t seq         = 0;
    uint32_t last_hb_ms  = 0;
    uint32_t last_log_ms = 0;
    bool     hb_state    = false;

    for (;;)
    {
#if (TP_TARGET_KBPS > 0u)
        /* Token-bucket pacing against wall-clock, using the recorder's exact
         * offered-byte counter (see Nucleo_H723_Throughput for rationale). */
        uint64_t budget_bytes = ((uint64_t) TP_TARGET_KBPS * 1024ull * (uint64_t) g_ms) / 1000ull;
        if ((uint64_t) _VA_TP.offeredBytes < budget_bytes)
        {
            VA_LogTrace (TRACE_TP, sine_lookup (seq));
            ++seq;
        }
#else
        VA_LogTrace (TRACE_TP, sine_lookup (seq));
        ++seq;
#endif

        uint32_t now_ms = g_ms;

        /* Live-watch demo signals (globals above): advance on each ms tick;
         * the loop counter every pass. Pure RAM writes — nothing traced. */
        {
            static uint32_t lw_prev_ms = 0;
            static uint32_t lw_chirp_phase = 0;                      /* cycles x 1000 */
            uint32_t lw_dms = now_ms - lw_prev_ms;
            if (lw_dms) {
                lw_prev_ms = now_ms;
                g_lw_sine_50hz = sine_smooth((now_ms * 1024u) / 5u);   /* 4096/20ms */
                g_lw_sine_5hz  = sine_smooth((now_ms * 512u) / 25u);   /* 4096/200ms */
                uint32_t lw_f = 1u + ((now_ms % 4000u) * 59u) / 4000u;
                lw_chirp_phase += lw_f * lw_dms;
                g_lw_chirp = sine_smooth((uint32_t)(((uint64_t) lw_chirp_phase * 4096u) / 1000u));
            }
            ++g_lw_loops;
        }

        if (now_ms - last_hb_ms >= 1000u)
        {
            last_hb_ms = now_ms;
            hb_state = ! hb_state;
            GPIOB->ODR ^= GPIO_ODR_OD0;               /* green LD1 blinks with the heartbeat */
            VA_LogToggle (TRACE_HB, hb_state);
        }

        if (now_ms - last_log_ms >= 5000u)
        {
            last_log_ms = now_ms;
            VA_LogString (TRACE_LOG, "Nucleo-F767 throughput benchmark running");
        }

        VA_TickOverflowCheck();
    }
}
