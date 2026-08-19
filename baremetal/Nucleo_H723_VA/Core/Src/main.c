/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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

/* ── ViewAlyzer demo instrumentation ──────────────────────────────────────
 * This project streams over the RAM buffer transport (VA_TRANSPORT =
 * RAM_BUFFER, set in CMakeLists.txt): the recorder owns a ring buffer in
 * RAM and the ViewAlyzer app drains it through the on-board ST-Link with
 * non-intrusive memory reads. No SWO pin, no SEGGER RTT, no extra wiring.
 */

/* Trace / event IDs */
#define TRACE_SINE        42
#define TRACE_TICK        43
#define TRACE_LED         44
#define EVENT_WORK        45
#define TRACE_WORKLOAD    46
#define TRACE_LOG         1

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
 * A layered integer DSP chain that runs EVERY loop iteration (no HAL_Delay):
 * the point is an instruction trace worth looking at -- real call depth,
 * branches that depend on data, loopy leaves, and work at three cadences
 * (every sample / every 16 / every 256).  noinline keeps the call structure
 * visible in the flame instead of being folded away by the optimizer. */
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

/* 4x4 integer matrix multiply -- nested-loop block every 16 samples */
static int32_t mix_state[4] = { 1000, 2000, 3000, 4000 };

DEMO_FN static void matrix_mix4 (void)
{
    static const int16_t coef[4][4] = {
        {  9, -3,  1,  0 },
        { -3,  9, -3,  1 },
        {  1, -3,  9, -3 },
        {  0,  1, -3,  9 },
    };
    int32_t out[4];
    for (int r = 0; r < 4; ++r)
    {
        int64_t acc = 0;
        for (int c = 0; c < 4; ++c)
            acc += (int64_t) coef[r][c] * mix_state[c];
        out[r] = (int32_t) (acc >> 3);
    }
    for (int r = 0; r < 4; ++r)
        mix_state[r] = out[r];
}

/* Bitwise CRC-32 over the histogram -- rare, loop-heavy block every 256 samples */
DEMO_FN static uint32_t crc32_block (const void* data, uint32_t len)
{
    const uint8_t* p = (const uint8_t*) data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (crc & 1u)));
    }
    return ~crc;
}

/* One pipeline pass: the whole chain, top of the demo call tree */
DEMO_FN static int32_t process_sample (uint32_t step)
{
    int32_t raw      = sensor_read (step);
    int32_t filtered = filter_biquad (raw);
    int32_t env      = envelope_track (filtered);
    uint32_t band    = classify (env);
    stats_update (band, filtered, step);
    if ((step & 15u) == 0u)  matrix_mix4();
    if ((step & 255u) == 0u) (void) crc32_block (band_hist, sizeof band_hist);
    return env;
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

  /* Start the recorder. The RAM buffer transport needs no other setup —
   * the host app finds the control block in RAM on its own. */
  VA_Init(SystemCoreClock);

  VA_RegisterUserTrace(TRACE_SINE,     "Sine Wave",     VA_USER_TYPE_GRAPH);
  VA_RegisterUserTrace(TRACE_TICK,     "Tick Counter",  VA_USER_TYPE_GRAPH);
  VA_RegisterUserTrace(TRACE_LED,      "LED Toggle",    VA_USER_TYPE_TOGGLE);
  VA_RegisterUserTrace(TRACE_WORKLOAD, "Workload",      VA_USER_TYPE_BAR);
  VA_RegisterUserEvent(EVENT_WORK,     "Work Block");

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

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t step = 0;
  bool led_state = false;
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* The pipeline runs EVERY iteration (no HAL_Delay: an on-chip trace
     * snapshot then always lands in real code, never in an idle poll).
     * Only every 256th pass is wrapped in the VA event -- at full speed the
     * loop runs ~100k+ passes/s and per-pass events would both flood the
     * recorder ring and make the emit path dominate the instruction trace. */
    bool traced_pass = (step & 255u) == 0u;
    if (traced_pass) VA_EVENT_START(EVENT_WORK);
    int32_t env = process_sample(step);
    if (traced_pass) VA_EVENT_END(EVENT_WORK);

    /* VA logging is decimated -- the loop is ~1000x faster without the
     * delay, and the recorder's ring must not be flooded. */
    if ((step & 1023u) == 0u)
    {
      VA_LogTrace(TRACE_SINE, sine_lookup(step >> 10));
      VA_LogTrace(TRACE_TICK, (int32_t) (HAL_GetTick() % 1000u));
      VA_LogTrace(TRACE_WORKLOAD, env);
    }

    if ((step & 32767u) == 0u)
    {
      led_state = ! led_state;
      BSP_LED_Toggle(LED_GREEN);
      VA_LogToggle(TRACE_LED, led_state);
    }

    if ((step & 0x7FFFFu) == 0u)
      VA_LogString(TRACE_LOG, "Nucleo-H723 RAM buffer demo alive");

    VA_TickOverflowCheck();
    ++step;
  }
  /* USER CODE END 3 */
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
