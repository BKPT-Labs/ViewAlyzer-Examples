#pragma once
/* ViewAlyzer recorder configuration for the Nucleo-C071RB (Cortex-M0+).
 * Included first by ViewAlyzerConfig.h via -DVA_CONFIG_HEADER=va_config.h.
 *
 * Cortex-M0+ has no DWT cycle counter and no ITM, so timestamps come from a
 * free-running timer (TIM3, 1 MHz, 16-bit) and the transport is the RAM
 * buffer the on-board ST-LINK drains with memory reads. */
#define VA_TIMESTAMP_SOURCE CUSTOM_TIMER
#define VA_TIMER_BITS       16
#define VA_TRANSPORT        RAM_BUFFER
/* 24 KB of SRAM on this part: an 8 KB ring still leaves plenty for the
 * stack and the recorder's own state. */
#define VA_RAMBUF_SIZE      8192u
