/**
 * @file viewalyzer_cobs.h
 * @brief COBS (Consistent Overhead Byte Stuffing) encoder for ViewAlyzer UDP transport.
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

#ifndef VIEWALYZER_COBS_H
#define VIEWALYZER_COBS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * COBS-encode a packet and append a 0x00 frame delimiter.
 *
 * The encoded output is guaranteed to contain no 0x00 bytes except
 * for the final delimiter. Overhead is ~1 byte per 254 input bytes
 * plus the trailing 0x00.
 *
 * @param input     Raw packet bytes to encode.
 * @param in_len    Length of input in bytes.
 * @param output    Buffer to write encoded bytes into.
 *                  Must be at least va_cobs_max_encoded_len(in_len) bytes.
 * @return          Number of bytes written to output (including the 0x00 delimiter).
 */
size_t va_cobs_encode(const uint8_t *input, size_t in_len, uint8_t *output);

/**
 * Returns the worst-case encoded length for a given input length.
 * Use this to size your output buffer.
 */
static inline size_t va_cobs_max_encoded_len(size_t in_len)
{
    /* overhead: 1 byte per 254 input bytes + 1 code byte + 1 delimiter */
    return in_len + (in_len / 254) + 2;
}

#ifdef __cplusplus
}
#endif

#endif /* VIEWALYZER_COBS_H */
