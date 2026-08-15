/**
 * Nucleo-F103RB — ViewAlyzer transport throughput benchmark (bare-metal).
 *
 * Cortex-M3 @ 72 MHz sibling of the Nucleo_H723_Throughput project: the
 * same paced saturation loop and _VA_TP offered/dropped accounting
 * (-DVA_TP_TEST=1), aimed at the board's on-board ST-LINK (a V2-1 — the
 * measured ceilings sit well below an ST-LINK V3's):
 *
 *     -DVA_TRANSPORT=RAM_BUFFER   probe memory-read RAM ring (default)
 *     -DVA_TRANSPORT=ARM_ITM      ITM / SWO trace pin
 *
 * Offered load: -DTP_TARGET_KBPS=<n> KiB/s (0 = unbounded blast). The default
 * saturates every debug-probe transport, so the host's delivered KB/s reading
 * is the transport ceiling.
 *
 * Bring-up is a HAL clock config (8 MHz HSE bypass from the ST-LINK MCO ->
 * PLL x9 -> 72 MHz); everything else runs on plain CMSIS registers. Note the
 * F103's 20 KB SRAM: the recorder's default 8 KB RAM ring is a large slice
 * of it — realistic for this class of part.
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

/* ── SysTick: HAL 1 kHz tick + a traced ISR band on the timeline ── */
void SysTick_Handler (void)
{
    VA_LogISRStart (VA_ISR_ID_SYSTICK);
    HAL_IncTick();
    VA_LogISREnd (VA_ISR_ID_SYSTICK);
}

/* ── Clock: 8 MHz HSE (ST-LINK MCO bypass) * 9 = 72 MHz (2 WS) ── */
static void SystemClock_Config (void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;      /* 8 MHz from the ST-LINK MCO */
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;      /* 8 * 9 = 72 MHz */
    if (HAL_RCC_OscConfig (&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig (&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
        Error_Handler();
}

/* Green LD2 on the Nucleo-F103RB (PA5) — a visible heartbeat so a flashed
 * board doesn't look dead (everything else is only visible in the trace). */
static void led_init (void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    (void) RCC->APB2ENR;                              /* dummy read: let the clock settle */
    /* PA5: general-purpose push-pull output, 2 MHz (CRL nibble 5 = 0b0010) */
    GPIOA->CRL = (GPIOA->CRL & ~(0xFu << 20)) | (0x2u << 20);
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
        uint64_t budget_bytes = ((uint64_t) TP_TARGET_KBPS * 1024ull * (uint64_t) HAL_GetTick()) / 1000ull;
        if ((uint64_t) _VA_TP.offeredBytes < budget_bytes)
        {
            VA_LogTrace (TRACE_TP, sine_lookup (seq));
            ++seq;
        }
#else
        VA_LogTrace (TRACE_TP, sine_lookup (seq));
        ++seq;
#endif

        uint32_t now_ms = HAL_GetTick();

        if (now_ms - last_hb_ms >= 1000u)
        {
            last_hb_ms = now_ms;
            hb_state = ! hb_state;
            GPIOA->ODR ^= GPIO_ODR_ODR5;              /* green LD2 blinks with the heartbeat */
            VA_LogToggle (TRACE_HB, hb_state);
        }

        if (now_ms - last_log_ms >= 5000u)
        {
            last_log_ms = now_ms;
            VA_LogString (TRACE_LOG, "Nucleo-F103 throughput benchmark running");
        }

        VA_TickOverflowCheck();
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
