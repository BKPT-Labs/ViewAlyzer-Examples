/**
 * Nucleo-U385RG-Q — ViewAlyzer transport throughput benchmark (bare-metal).
 *
 * Cortex-M33 @ 96 MHz sibling of the Nucleo_H723_Throughput project: the
 * same paced saturation loop and _VA_TP offered/dropped accounting
 * (-DVA_TP_TEST=1), aimed at the board's on-board ST-LINK V3:
 *
 *     -DVA_TRANSPORT=RAM_BUFFER   probe memory-read RAM ring (default)
 *     -DVA_TRANSPORT=ARM_ITM      ITM / SWO trace pin
 *
 * Offered load: -DTP_TARGET_KBPS=<n> KiB/s (0 = unbounded blast). The default
 * saturates every debug-probe transport, so the host's delivered KB/s reading
 * is the transport ceiling.
 *
 * Bring-up is a HAL clock config (MSIS RC0 -> 96 MHz, EPOD booster, VOS1);
 * everything else runs on plain CMSIS registers. The ICACHE is enabled —
 * instruction cache only, it does not affect probe reads of the RAM ring.
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

/* ── Clock: MSIS RC0 (96 MHz) as SYSCLK — EPOD booster + VOS1, 2 WS ── */
static void SystemClock_Config (void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* The PWR block is bus-clock-gated on the U3 — enable it before any
     * regulator / EPOD access or every PWR call silently fails. */
    __HAL_RCC_PWR_CLK_ENABLE();

    if (HAL_RCCEx_EpodBoosterClkConfig (RCC_EPODBOOSTER_SOURCE_MSIS,
                                        RCC_EPODBOOSTER_DIV1) != HAL_OK)
        Error_Handler();
    if (HAL_PWREx_EnableEpodBooster() != HAL_OK)
        Error_Handler();
    if (HAL_PWREx_ControlVoltageScaling (PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
        Error_Handler();

    __HAL_FLASH_SET_LATENCY (FLASH_LATENCY_2);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_MSIS;
    RCC_OscInitStruct.MSISState = RCC_MSI_ON;
    RCC_OscInitStruct.MSISSource = RCC_MSI_RC0;         /* 96 MHz */
    RCC_OscInitStruct.MSISDiv = RCC_MSI_DIV1;
    if (HAL_RCC_OscConfig (&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2
                                | RCC_CLOCKTYPE_PCLK3;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSIS;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig (&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
        Error_Handler();
}

/* Green LD2 on the Nucleo-U385RG-Q (PA5) — a visible heartbeat so a flashed
 * board doesn't look dead (everything else is only visible in the trace). */
static void led_init (void)
{
    RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN;
    (void) RCC->AHB2ENR1;                             /* dummy read: let the clock settle */
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE5_Msk) | (1u << GPIO_MODER_MODE5_Pos);
}

int main (void)
{
    HAL_Init();
    SystemClock_Config();
    HAL_ICACHE_Enable();
    /* Keep the debug interface clocked in low-power modes (ARMv8-M has no
     * DBG_SLEEP bit; sleep-mode debug always works on this part). */
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
            GPIOA->ODR ^= GPIO_ODR_OD5;               /* green LD2 blinks with the heartbeat */
            VA_LogToggle (TRACE_HB, hb_state);
        }

        if (now_ms - last_log_ms >= 5000u)
        {
            last_log_ms = now_ms;
            VA_LogString (TRACE_LOG, "Nucleo-U385 throughput benchmark running");
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
void SecureFault_Handler (void){ for (;;) {} }
void SVC_Handler (void)        {}
void DebugMon_Handler (void)   {}
void PendSV_Handler (void)     {}
