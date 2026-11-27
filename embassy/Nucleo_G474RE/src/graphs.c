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
#include "ViewAlyzer.h"

/* The Embassy adapter reserves numeric trace IDs 2 and 3 for poll wakes. */
enum {
    RAW_INPUT = 4,
    FILTERED_INPUT,
    APPLIED_TARGET,
    CONTROL_ERROR,
};

void demo_register_graphs(void)
{
    VA_RegisterUserTrace(RAW_INPUT, "Raw input", VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(FILTERED_INPUT, "Filtered input", VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(APPLIED_TARGET, "Applied target", VA_USER_TYPE_GRAPH);
    VA_RegisterUserTrace(CONTROL_ERROR, "Control error", VA_USER_TYPE_GRAPH);
}

void demo_log_raw(int32_t value)
{
    VA_LogTrace(RAW_INPUT, value);
}

void demo_log_filtered(int32_t value)
{
    VA_LogTrace(FILTERED_INPUT, value);
}

void demo_log_control(int32_t target, int32_t error)
{
    VA_LogTrace(APPLIED_TARGET, target);
    VA_LogTrace(CONTROL_ERROR, error);
}
