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
#include CMSIS_device_header
#include "cmsis_os2.h"
#include "ViewAlyzer.h"

extern void rtx5_workload_init(void);
extern volatile uint32_t rtx5_example_failures;
volatile uint32_t rtx5_example_pre_kernel_fp;

static void clock_init(void)
{
#if defined(STM32G474xx)
    /* 16 MHz HSI / 4 * 40 / 2 = 80 MHz, within voltage-range-1 limits
       without the boost-mode transition required above 150 MHz. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    PWR->CR1 = (PWR->CR1 & ~PWR_CR1_VOS) | PWR_CR1_VOS_0;
    while (PWR->SR2 & PWR_SR2_VOSF) {}
    /* Preserve DBG_SWEN and the flash accelerator settings. Clearing
       DBG_SWEN disconnects SWD as soon as this register is written. */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) |
        FLASH_ACR_LATENCY_4WS | FLASH_ACR_PRFTEN;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS) {}
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | (3u << RCC_PLLCFGR_PLLM_Pos) |
        (40u << RCC_PLLCFGR_PLLN_Pos) | RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_SW)) |
        RCC_CFGR_SW_PLL;
#else
    /* Board-local HSI; no dependency on an ST-Link MCO cable. This is the
       same 216 MHz setup used by the qualified F767 ThreadX example. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR1 |= PWR_CR1_VOS;
    RCC->PLLCFGR = 16u | (432u << RCC_PLLCFGR_PLLN_Pos) | (9u << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}
    PWR->CR1 |= PWR_CR1_ODEN;
    while (!(PWR->CSR1 & PWR_CSR1_ODRDY)) {}
    PWR->CR1 |= PWR_CR1_ODSWEN;
    while (!(PWR->CSR1 & PWR_CSR1_ODSWRDY)) {}
    FLASH->ACR = FLASH_ACR_LATENCY_7WS | FLASH_ACR_PRFTEN;
    RCC->CFGR = RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2 | RCC_CFGR_SW_PLL;
#endif
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}
    SystemCoreClockUpdate();
}

int main(void)
{
    /* Data caches stay off so RAM/RTT producer writes are visible to probes. */
    clock_init();
#if defined(STM32G474xx)
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
#else
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
#endif
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 6)) | (2u << 6);
    GPIOB->OSPEEDR |= 3u << 6;
    GPIOB->AFR[0] &= ~(15u << 12);
    DBGMCU->CR = (DBGMCU->CR & ~DBGMCU_CR_TRACE_MODE) | DBGMCU_CR_TRACE_IOEN;
    uint32_t saved_fpscr, test_fpscr = 0x01400000u, observed_fpscr;
    __asm volatile("vmrs %0, fpscr" : "=r"(saved_fpscr));
    /* Exercise lazy FP stacking on MSP while curr == next == NULL. The
       recorder's deleted-thread cleanup must leave this state untouched. */
    __asm volatile("vmsr fpscr, %0\nvmov.f32 s0, #1.0" :: "r"(test_fpscr) : "s0");
    VA_Init(SystemCoreClock);
    if (osKernelInitialize() != osOK) for (;;) {}
    rtx5_workload_init();
    __asm volatile("vmrs %0, fpscr" : "=r"(observed_fpscr));
    if ((observed_fpscr & 0x03c00000u) != test_fpscr) ++rtx5_example_failures;
    else rtx5_example_pre_kernel_fp = 1;
    __asm volatile("vmsr fpscr, %0" :: "r"(saved_fpscr));
    (void)osKernelStart();
    for (;;) {}
}
