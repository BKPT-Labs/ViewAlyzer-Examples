/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : ViewAlyzer transport throughput benchmark (bare-metal)
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ViewAlyzer.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

COM_InitTypeDef BspCOMInit;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ── ViewAlyzer transport throughput benchmark ────────────────────────────
 * This project answers one question: how many trace bytes per second can a
 * given ViewAlyzer transport actually sustain from this target to the host?
 *
 * It emits fixed-size trace packets from the main loop as fast as a paced
 * "offered load" allows.  The offered rate (TP_TARGET_KBPS) is set well above
 * any real transport ceiling, so the pipe *saturates*:
 *
 *   - the ViewAlyzer app's live "delivered KB/s" is the sustained throughput,
 *   - the recorder's _VA_TP counter (offered vs. dropped, enabled by
 *     -DVA_TP_TEST=1) plus the RAM-buffer drop counter reveal exactly how
 *     much load the transport had to shed to hold that rate.
 *
 * The transport is a CMake cache variable (VA_TRANSPORT):
 *     RAM_BUFFER (default) — ST-Link memory-read ring, no extra wiring
 *     ARM_ITM              — SWO pin (configure -DVA_TRANSPORT=ARM_ITM)
 *
 * Offered load: -DTP_TARGET_KBPS=<n>  (KiB/s, 0 = unbounded blast — note the
 * 32-bit offered-byte counter wraps within seconds when unbounded, so prefer
 * a bounded rate for clean offered/drop accounting).  Default 8000 KiB/s is
 * far above both SWO (~200 KB/s @ 2 MHz) and the ST-Link RAM-buffer ceiling,
 * so both transports saturate while the offered counter stays meaningful.
 */

#ifndef TP_TARGET_KBPS
#define TP_TARGET_KBPS 8000u          /* offered-load ceiling in KiB/s (0 = unbounded) */
#endif

/* ── Live-watch speed demo ────────────────────────────────────────────────
 * These globals exist for the IDE's LIVE WATCH — raw memory polling through
 * the debug port while the core runs, no trace transport involved. The BKPT
 * live channel samples a 4-byte variable at ~2.4 kHz (0.4 ms/read measured),
 * so these move fast enough to separate the debuggers from the toys:
 *   g_lw_sine_50hz — 50 Hz sine, ±1000. Needs >100 Hz sampling to render at
 *                    all; a ~1-10 Hz "live expression" poller shows noise,
 *                    a kHz-class poller draws the waveform.
 *   g_lw_sine_5hz  — 5 Hz sine: the fairness baseline any tool can follow.
 *   g_lw_chirp     — sine sweeping 1→60 Hz every 4 s: the money shot — the
 *                    exact frequency where a viewer's polling rate gives up
 *                    is visible as the point the waveform dissolves.
 *   g_lw_loops     — raw main-loop pass counter (millions/s on this core).
 */
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

/* Trace IDs */
#define TRACE_TP     50               /* the throughput payload trace       */
#define TRACE_HB     51               /* 1 Hz heartbeat toggle (visual)      */
#define TRACE_LOG     1

/* Quarter-wave sine table (0..90 deg, scaled 0..1000) — gives the payload
 * trace a recognizable shape in the viewer without pulling in libm. */
static const int16_t sine_quarter[16] = {
    0, 98, 195, 290, 383, 471, 556, 634, 707, 773, 831, 881, 924, 957, 981, 995
};

static int16_t sine_lookup(uint32_t idx)
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

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */

  /* Start the recorder. Whichever transport was compiled in, the host finds
   * the stream on its own (RAM-buffer control block scan, or SWO/ITM). */
  VA_Init(SystemCoreClock);

  VA_RegisterUserTrace(TRACE_TP, "Throughput Pay", VA_USER_TYPE_GRAPH);
  VA_RegisterUserTrace(TRACE_HB, "Heartbeat",          VA_USER_TYPE_TOGGLE);

  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);
  BSP_LED_Init(LED_YELLOW);
  BSP_LED_Init(LED_RED);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Initialize COM1 port (115200, 8 bits (7-bit data + 1 stop bit), no parity */
  BspCOMInit.BaudRate   = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits   = COM_STOPBITS_1;
  BspCOMInit.Parity     = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN WHILE */
  const uint32_t start_ms = HAL_GetTick();
  uint32_t seq          = 0;
  uint32_t last_hb_ms   = start_ms;
  uint32_t last_log_ms  = start_ms;
  bool     hb_state     = false;

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#if (TP_TARGET_KBPS > 0u)
    /* Token-bucket pacing against wall-clock: only emit while the recorder's
     * offered-byte total is still under the budget for the elapsed time. The
     * offered counter (_VA_TP.offeredBytes, from -DVA_TP_TEST=1) is the exact
     * byte count the transport was handed, so pacing needs no packet-size
     * guess. When the transport saturates and cannot drain, packets drop —
     * that shed load is exactly what this benchmark measures. */
    uint32_t elapsed_ms = HAL_GetTick() - start_ms;
    uint64_t budget_bytes = ((uint64_t) TP_TARGET_KBPS * 1024ull * (uint64_t) elapsed_ms) / 1000ull;
    if ((uint64_t) _VA_TP.offeredBytes < budget_bytes)
    {
      VA_LogTrace(TRACE_TP, sine_lookup(seq));
      ++seq;
    }
#else
    /* Unbounded blast: emit every iteration. Offered-byte counter wraps within
     * seconds — keep runs short and read delivered KB/s from the host. */
    VA_LogTrace(TRACE_TP, sine_lookup(seq));
    ++seq;
#endif

    /* 1 Hz heartbeat toggle + green LED — a low-rate landmark so the trace is
     * easy to eyeball; negligible against the payload byte rate. */
    uint32_t now_ms = HAL_GetTick();

    /* Live-watch demo signals (globals above): advance on each ms tick; the
     * loop counter every pass. Pure RAM writes — nothing traced, no pacing. */
    {
      static uint32_t lw_prev_ms = 0;
      static uint32_t lw_chirp_phase = 0;                       /* cycles x 1000 */
      uint32_t lw_dms = now_ms - lw_prev_ms;
      if (lw_dms) {
        lw_prev_ms = now_ms;
        g_lw_sine_50hz = sine_smooth((now_ms * 1024u) / 5u);   /* 4096/20ms */
        g_lw_sine_5hz  = sine_smooth((now_ms * 512u) / 25u);   /* 4096/200ms */
        uint32_t lw_f = 1u + ((now_ms % 4000u) * 59u) / 4000u;  /* 1 -> 60 Hz sweep */
        lw_chirp_phase += lw_f * lw_dms;
        g_lw_chirp = sine_smooth((uint32_t)(((uint64_t) lw_chirp_phase * 4096u) / 1000u));
      }
      ++g_lw_loops;
    }

    if (now_ms - last_hb_ms >= 1000u)
    {
      last_hb_ms = now_ms;
      hb_state = !hb_state;
      BSP_LED_Toggle(LED_GREEN);
      VA_LogToggle(TRACE_HB, hb_state);
    }

    if (now_ms - last_log_ms >= 5000u)
    {
      last_log_ms = now_ms;
      VA_LogString(TRACE_LOG, "Nucleo-H723 throughput benchmark running");
    }

    VA_TickOverflowCheck();
    /* USER CODE END 3 */
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = 64;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 12;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
