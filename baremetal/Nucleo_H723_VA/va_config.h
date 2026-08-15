/**
 * @file va_config.h
 * @brief Example ViewAlyzer configuration OUTSIDE the recorder tree.
 *
 * Build with -DVA_CONFIG_HEADER=va_config.h (a bare token, no quotes and no
 * angle brackets) and this file is included before every default in
 * ViewAlyzerConfig.h, so anything set here wins.
 *
 * Why not just edit ViewAlyzerConfig.h? Because the recorder is vendored into
 * a project by the package flow, and the next package update overwrites it.
 * A config header of your own survives that.
 *
 * This example is a bare-metal, user-instrumentation-only build: no RTOS
 * exists, so every RTOS category is dead weight, and of the user-side ones we
 * keep only what this firmware actually calls. Everything not named below
 * follows VA_TRACE_DEFAULT and is compiled out entirely: no events, no packet
 * builders, no registries, and no cost at the call sites (calls to a disabled
 * API still compile, and do not evaluate their arguments).
 *
 * Turn it on for this project with:
 *     cmake -B build -DVA_SELECTIVE_TRACING=ON
 */

#ifndef VA_CONFIG_H
#define VA_CONFIG_H

/* Nothing is traced unless it is named below. */
#define VA_TRACE_DEFAULT 0

/* What this firmware actually logs. */
#define VA_TRACE_USER_VALUES  1   /* VA_LogTrace / VA_LogTraceFloat / VA_LogToggle */
#define VA_TRACE_USER_EVENTS  1   /* VA_EVENT_START / VA_EVENT_END spans           */
#define VA_TRACE_ISRS         1   /* VA_LogISRStart / VA_LogISREnd                 */

/* Smaller registries to match: this build registers a handful of user ids and
   no RTOS objects at all. */
#define VA_MAX_USER_EVENTS 8

/* ── RAM buffer transport (VA_TRANSPORT=RAM_BUFFER, this project's default) ──
   The lines below show the recorder defaults; uncomment to change them.
   Note this header is only in the build when -DVA_SELECTIVE_TRACING=ON
   wires up -DVA_CONFIG_HEADER=va_config.h (see CMakeLists.txt). */

/* #define VA_RAMBUF_SIZE 8192u */
/* #define VA_RAMBUF_MODE VA_RAMBUF_MODE_DROP */

/* Placement of the ring + control block. This template boots with the
   caches DISABLED, so the default .bss placement is always coherent for
   probe reads. If you enable the D-cache (the H723 is a Cortex-M7), pin
   the ring to a non-cacheable MPU region or DTCM via a section from
   your linker script - the host finds it wherever it lands (ELF symbol
   or magic-tag scan): */
/* #define VA_RAMBUF_ATTRIBUTES __attribute__((section(".va_rambuf"))) */

#endif /* VA_CONFIG_H */
