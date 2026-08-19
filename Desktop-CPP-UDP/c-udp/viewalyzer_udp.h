/**
 * @file viewalyzer_udp.h
 * @brief ViewAlyzer UDP sender - core tracing over UDP with COBS framing.
 *
 * This is a lightweight, standalone C library for sending ViewAlyzer
 * trace data to the desktop app over UDP.  It has **no RTOS dependency**
 * and covers the generic "inner circle": int/float traces, string events,
 * toggles, and function entry/exit spans.
 *
 * For RTOS events (TaskSwitch, ISR, Semaphore, Mutex, Queue, etc.),
 * include "viewalyzer_udp_rtos.h" which extends this header.
 *
 * Usage:
 *   1. Call va_udp_init() with destination IP, port, and CPU frequency.
 *   2. Send setup packets: va_udp_send_trace_setup(), va_udp_send_function_map().
 *   3. In your loop, send events: va_udp_send_trace_int(), va_udp_send_trace_float(),
 *      va_udp_send_string(), va_udp_send_toggle(), va_udp_send_function().
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

#ifndef VIEWALYZER_UDP_H
#define VIEWALYZER_UDP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Core protocol constants ───────────────────────────────────────────── */

/* Wire protocol id (mirrors VA_WIRE_VERSION in ViewAlyzerRecorder/core).
   Format 1 = 32-bit timestamps + a per-packet sequence byte at offset 1 of
   every packet, stamped centrally by va_udp_emit(). */
#define VA_UDP_WIRE_VERSION         1

/* Core event type codes (lower 7 bits of type byte) */
#define VA_UDP_EVT_USER_TRACE       0x04  /* int32 value */
#define VA_UDP_EVT_USER_TOGGLE      0x0A
#define VA_UDP_EVT_USER_FUNCTION    0x0B
#define VA_UDP_EVT_STRING_EVENT     0x0D  /* variable-length string */
#define VA_UDP_EVT_FLOAT_TRACE      0x0E  /* IEEE 754 float value */
#define VA_UDP_EVT_SEQ_CHECKPOINT   0x16  /* absolute sequence checkpoint */

#define VA_UDP_FLAG_START           0x80  /* MSB: start/enter */

/* Core setup packet codes */
#define VA_UDP_SETUP_USER_TRACE        0x72
#define VA_UDP_SETUP_USER_FUNCTION_MAP 0x76
#define VA_UDP_SETUP_INFO              0x7F

/* Trace visualisation type hints (byte 2 of UserTrace setup).
   Mirrors VA_UserTraceType_t in core/ViewAlyzer.h - the firmware enum is
   the authority; do not invent values here. */
#define VA_UDP_TRACE_GRAPH      0
#define VA_UDP_TRACE_BAR        1
#define VA_UDP_TRACE_GAUGE      2
#define VA_UDP_TRACE_COUNTER    3
#define VA_UDP_TRACE_TABLE      4
#define VA_UDP_TRACE_HISTOGRAM  5
#define VA_UDP_TRACE_TOGGLE     6
#define VA_UDP_TRACE_TASK       7
#define VA_UDP_TRACE_ISR        8

/* Maximum string length for string events (2-byte length field, uint16_t LE) */
#define VA_UDP_MAX_STRING_LEN   1024

/* Max raw packet size (header + string) and COBS encode buffer size */
#define VA_UDP_MAX_PKT_LEN      (12 + VA_UDP_MAX_STRING_LEN)
#define VA_UDP_COBS_BUF_LEN     (VA_UDP_MAX_PKT_LEN + (VA_UDP_MAX_PKT_LEN / 254) + 2)

/* ── Context ───────────────────────────────────────────────────────────── */

/** Opaque handle - call va_udp_init() to populate. */
typedef struct va_udp_ctx va_udp_ctx_t;

/**
 * Raw-transport callback signature.
 * @param arg   User-supplied pointer (e.g. pointer to a driver wrapper).
 * @param data  COBS-encoded payload to transmit as a single UDP datagram.
 * @param len   Length of @p data in bytes.
 */
typedef void (*va_udp_send_fn)(void* arg, const uint8_t* data, size_t len);

/**
 * Initialise the UDP sender.
 *
 * @param dest_ip     Destination IP address (e.g. "127.0.0.1").
 * @param dest_port   Destination UDP port (e.g. 17200).
 * @param cpu_freq_hz CPU clock frequency in Hz (for the CLK setup packet).
 * @return            Heap-allocated context, or NULL on failure.
 *                    Free with va_udp_close().
 */
va_udp_ctx_t *va_udp_init(const char *dest_ip, uint16_t dest_port, uint32_t cpu_freq_hz);

/**
 * Set the raw send callback (replaces the default socket-based sendto).
 * Must be called after va_udp_init() and before any trace emission.
 */
void va_udp_set_send_fn(va_udp_ctx_t *ctx, va_udp_send_fn fn, void *arg);

/** Close the socket and free the context. */
void va_udp_close(va_udp_ctx_t *ctx);

/* ── Core setup packets ────────────────────────────────────────────────── */

/** Send the session prelude: sync marker, SES:START, sequence checkpoint,
 *  and the CLK info packet (the mirror of the firmware's VA_Init).
 *  Call once at startup. */
void va_udp_send_sync_and_clock(va_udp_ctx_t *ctx);

/** Register a user trace channel (id, display type, name). */
void va_udp_send_trace_setup(va_udp_ctx_t *ctx, uint8_t trace_id,
                             uint8_t trace_type, const char *name);

/** Register a user event or span name. */
void va_udp_send_function_map(va_udp_ctx_t *ctx, uint8_t func_id, const char *name);

/* ── Core event packets ────────────────────────────────────────────────── */

/** User trace with a signed 32-bit integer value. */
void va_udp_send_trace_int(va_udp_ctx_t *ctx, uint8_t trace_id,
                           uint64_t timestamp, int32_t value);

/** User trace with an IEEE 754 float value (FloatTrace 0x0E). */
void va_udp_send_trace_float(va_udp_ctx_t *ctx, uint8_t trace_id,
                             uint64_t timestamp, float value);

/** Boolean toggle state change. */
void va_udp_send_toggle(va_udp_ctx_t *ctx, uint8_t toggle_id,
                        uint64_t timestamp, bool state);

/** User event or span start/end marker. */
void va_udp_send_function(va_udp_ctx_t *ctx, uint8_t func_id,
                          bool is_entry, uint64_t timestamp);

/** Variable-length string message (max 200 chars). */
void va_udp_send_string(va_udp_ctx_t *ctx, uint8_t msg_id,
                        uint64_t timestamp, const char *message);

/* ── Batching ───────────────────────────────────────────────────────────── */

/**
 * Begin accumulating packets into an internal buffer instead of sending
 * each one immediately.  Call va_udp_batch_flush() to send the accumulated
 * buffer as a single UDP datagram (or a small number of MTU-sized chunks).
 *
 * Supports nesting - only the outermost flush actually sends.
 */
void va_udp_batch_begin(va_udp_ctx_t *ctx);

/**
 * Flush (send) the accumulated batch buffer and return to immediate mode.
 */
void va_udp_batch_flush(va_udp_ctx_t *ctx);

/* ── Low-level helpers (for advanced use / RTOS extension) ─────────────── */

/**
 * COBS-encode a raw byte sequence and send it over the context's UDP socket
 * WITHOUT stamping a sequence byte. Only for sync markers (which are not
 * packets and consume no sequence number). If batching is active, the
 * encoded frame is appended to the batch buffer instead of being sent
 * immediately.
 */
void va_udp_send_raw_framed(va_udp_ctx_t *ctx, const uint8_t *pkt, size_t pkt_len);

/**
 * Stamp the packet's sequence slot (offset 1) and send it framed. Builders
 * must lay packets out as [type][seq][rest...] with the seq byte left for
 * this function to fill. Used internally and by viewalyzer_udp_rtos.c.
 */
void va_udp_emit(va_udp_ctx_t *ctx, uint8_t *pkt, size_t pkt_len);

/**
 * Send a name-mapping setup packet: [code][seq][id][len][name...].
 * Used internally and by viewalyzer_udp_rtos.c.
 */
void va_udp_send_name_setup(va_udp_ctx_t *ctx, uint8_t code, uint8_t id, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* VIEWALYZER_UDP_H */
