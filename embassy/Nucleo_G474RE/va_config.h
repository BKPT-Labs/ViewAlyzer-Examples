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
#pragma once
#define VA_TRACE_USER_VALUES 1
#define VA_EMBASSY_SELF_WAKE_TRACE_ID 2
#define VA_EMBASSY_IRQ_WAKE_TRACE_ID  3
#include "VA_EmbassyConfig.h"
#define VA_TRANSPORT RAM_BUFFER
#define VA_RAMBUF_SIZE 32768u
