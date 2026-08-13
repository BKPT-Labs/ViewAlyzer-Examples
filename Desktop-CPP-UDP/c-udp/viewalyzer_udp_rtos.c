/**
 * @file viewalyzer_udp_rtos.c
 * @brief ViewAlyzer UDP RTOS extension - implementation.
 *
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

#include "viewalyzer_udp_rtos.h"

#include <string.h>

/* ── Helpers (same layout as core, duplicated to keep this translation unit self-contained) */

static void write_i32_le(uint8_t *buf, int32_t v)  { memcpy(buf, &v, 4); }
static void write_u32_le(uint8_t *buf, uint32_t v) { memcpy(buf, &v, 4); }

/* Wire timestamps are the low 32 bits of the cycle counter. */
static uint32_t ts_mask(uint64_t ts) { return (uint32_t)ts; }

/* ── RTOS setup packets ───────────────────────────────────────────────── */

void va_udp_send_task_map(va_udp_ctx_t *ctx, uint8_t task_id, const char *name)
{
    va_udp_send_name_setup(ctx, VA_UDP_SETUP_TASK_MAP, task_id, name);
}

void va_udp_send_isr_map(va_udp_ctx_t *ctx, uint8_t isr_id, const char *name)
{
    va_udp_send_name_setup(ctx, VA_UDP_SETUP_ISR_MAP, isr_id, name);
}

void va_udp_send_semaphore_map(va_udp_ctx_t *ctx, uint8_t sem_id, const char *name)
{
    va_udp_send_name_setup(ctx, VA_UDP_SETUP_SEMAPHORE_MAP, sem_id, name);
}

void va_udp_send_mutex_map(va_udp_ctx_t *ctx, uint8_t mutex_id, const char *name)
{
    va_udp_send_name_setup(ctx, VA_UDP_SETUP_MUTEX_MAP, mutex_id, name);
}

void va_udp_send_queue_map(va_udp_ctx_t *ctx, uint8_t queue_id, const char *name)
{
    va_udp_send_name_setup(ctx, VA_UDP_SETUP_QUEUE_MAP, queue_id, name);
}

/* ── RTOS event packets ───────────────────────────────────────────────── */

void va_udp_send_task_switch(va_udp_ctx_t *ctx, uint8_t task_id,
                             bool is_enter, uint64_t timestamp)
{
    uint8_t pkt[7];
    pkt[0] = VA_UDP_EVT_TASK_SWITCH | (is_enter ? VA_UDP_FLAG_START : 0);
    pkt[2] = task_id;
    write_u32_le(&pkt[3], ts_mask(timestamp));
    va_udp_emit(ctx, pkt, 7);
}

void va_udp_send_isr(va_udp_ctx_t *ctx, uint8_t isr_id,
                     bool is_enter, uint64_t timestamp)
{
    uint8_t pkt[7];
    pkt[0] = VA_UDP_EVT_ISR | (is_enter ? VA_UDP_FLAG_START : 0);
    pkt[2] = isr_id;
    write_u32_le(&pkt[3], ts_mask(timestamp));
    va_udp_emit(ctx, pkt, 7);
}

void va_udp_send_task_create(va_udp_ctx_t *ctx, uint8_t task_id, uint64_t timestamp,
                             int32_t priority, int32_t base_priority, int32_t stack_size)
{
    uint8_t pkt[19];
    pkt[0] = VA_UDP_EVT_TASK_CREATE | VA_UDP_FLAG_START;
    pkt[2] = task_id;
    write_u32_le(&pkt[3],  ts_mask(timestamp));
    write_i32_le(&pkt[7],  priority);
    write_i32_le(&pkt[11], base_priority);
    write_i32_le(&pkt[15], stack_size);
    va_udp_emit(ctx, pkt, 19);
}

void va_udp_send_task_notify(va_udp_ctx_t *ctx, uint8_t src_task_id,
                             uint8_t dst_task_id, uint64_t timestamp, int32_t value)
{
    uint8_t pkt[12];
    pkt[0] = VA_UDP_EVT_TASK_NOTIFY | VA_UDP_FLAG_START;
    pkt[2] = src_task_id;
    pkt[3] = dst_task_id;
    write_u32_le(&pkt[4], ts_mask(timestamp));
    write_i32_le(&pkt[8], value);
    va_udp_emit(ctx, pkt, 12);
}

void va_udp_send_semaphore(va_udp_ctx_t *ctx, uint8_t sem_id,
                           bool is_give, uint64_t timestamp)
{
    uint8_t pkt[7];
    pkt[0] = VA_UDP_EVT_SEMAPHORE | (is_give ? VA_UDP_FLAG_START : 0);
    pkt[2] = sem_id;
    write_u32_le(&pkt[3], ts_mask(timestamp));
    va_udp_emit(ctx, pkt, 7);
}

void va_udp_send_mutex(va_udp_ctx_t *ctx, uint8_t mutex_id,
                       bool is_acquire, uint64_t timestamp)
{
    uint8_t pkt[7];
    pkt[0] = VA_UDP_EVT_MUTEX | (is_acquire ? VA_UDP_FLAG_START : 0);
    pkt[2] = mutex_id;
    write_u32_le(&pkt[3], ts_mask(timestamp));
    va_udp_emit(ctx, pkt, 7);
}

void va_udp_send_queue(va_udp_ctx_t *ctx, uint8_t queue_id,
                       bool is_send, uint64_t timestamp)
{
    uint8_t pkt[7];
    pkt[0] = VA_UDP_EVT_QUEUE | (is_send ? VA_UDP_FLAG_START : 0);
    pkt[2] = queue_id;
    write_u32_le(&pkt[3], ts_mask(timestamp));
    va_udp_emit(ctx, pkt, 7);
}

void va_udp_send_stack_usage(va_udp_ctx_t *ctx, uint8_t task_id, uint64_t timestamp,
                             uint32_t used_bytes, uint32_t total_bytes)
{
    uint8_t pkt[15];
    pkt[0] = VA_UDP_EVT_TASK_STACK_USAGE;
    pkt[2] = task_id;
    write_u32_le(&pkt[3],  ts_mask(timestamp));
    write_u32_le(&pkt[7],  used_bytes);
    write_u32_le(&pkt[11], total_bytes);
    va_udp_emit(ctx, pkt, 15);
}

void va_udp_send_mutex_contention(va_udp_ctx_t *ctx, uint8_t mutex_id,
                                  uint8_t waiting_task_id, uint8_t holder_task_id,
                                  uint64_t timestamp)
{
    uint8_t pkt[9];
    pkt[0] = VA_UDP_EVT_MUTEX_CONTENTION;
    pkt[2] = mutex_id;
    pkt[3] = waiting_task_id;
    pkt[4] = holder_task_id;
    write_u32_le(&pkt[5], ts_mask(timestamp));
    va_udp_emit(ctx, pkt, 9);
}
