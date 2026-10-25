/*
 * Copyright 2025-2026 BKPT, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "stm32g4xx.h"
#include "tx_api.h"
#include "ViewAlyzer.h"

/* ST's unchanged main configures HSI/PLL at 170 MHz before this boundary. */
void MX_ThreadX_Init(void)
{
    /* HAL needs TIM6 during setup; ThreadX owns the subsequent tick source. */
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
#if VA_TRANSPORT == ARM_ITM
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 6)) | (2u << 6);
    GPIOB->OSPEEDR |= 3u << 6;
    GPIOB->AFR[0] &= ~(15u << 12);
    DBGMCU->CR = (DBGMCU->CR & ~DBGMCU_CR_TRACE_MODE) | DBGMCU_CR_TRACE_IOEN;
#endif
    VA_Init(SystemCoreClock);
    VA_RegisterUserTrace((uint8_t)(16 + EXTI0_IRQn), "TxSoftwareIRQ", VA_USER_TYPE_ISR);
    VA_LogString(0, "THREADX:G474RE:v1");
    tx_kernel_enter();
}
