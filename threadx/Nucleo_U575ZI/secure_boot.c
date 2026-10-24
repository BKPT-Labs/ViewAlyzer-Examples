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
/* Reuse the pinned ST GTZC donor's partition and NS gateways. The same paired
 * image layout is used by the rack's U575 CI fixture. Keep ST's source intact. */
#define main st_gtzc_main
#include "Secure/Src/main.c"
#undef main

int main(void)
{
    SCB->SHCSR |= SCB_SHCSR_SECUREFAULTENA_Msk;
    HAL_Init();
    SystemPower_Config();
    MX_GTZC_S_Init();
    MX_GPIO_Init();
    /* The NonSecure ThreadX donor configures and enables ICACHE itself;
     * enabling the secure donor's cache here would make that setup fail. */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_ConfigPinAttributes(GPIOB, GPIO_PIN_3, GPIO_PIN_NSEC);
    NVIC_SetTargetState(EXTI0_IRQn);
    NVIC_SetTargetState(TIM6_IRQn);
    HAL_SuspendTick();
    NonSecure_Init();
    for (;;) {}
}
