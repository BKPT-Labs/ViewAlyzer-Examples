# Nucleo-F767ZI — J-Link Transport Throughput Benchmark

Bare-metal, **CMSIS-only** (no HAL/BSP) benchmark measuring how many trace
bytes per second a **J-Link** can sustain from a Nucleo-F767ZI (Cortex-M7 @
96 MHz, HSI-PLL) to the host. Sibling of `Nucleo_H723_Throughput` — same
paced saturation loop and `_VA_TP` offered/dropped accounting — but with the
transport switch aimed at J-Link use cases:

- **RAM buffer** (default) — the probe-agnostic ring, drained by memory reads
- **SEGGER RTT** (`-DVA_TRANSPORT=JLINK_RTT`) — vendored `SEGGER_RTT/` sources

All RAM lives in **DTCM** (never cached, inside the app's default scan
window). Caches stay off. If your own firmware enables the D-cache or
places `.bss` elsewhere, pin the ring to a non-cacheable region with
`VA_RAMBUF_ATTRIBUTES` (a section attribute; set it via the
`VA_CONFIG_HEADER` pattern, see `core/va_config_template.h`). CMSIS
headers are vendored under `Drivers/` (`cmsis-core` + `cmsis-device-f7`,
BSD-3 ST/ARM licenses).

## Build & flash (J-Link)

```sh
cmake -S . -B build/rambuf -G Ninja --toolchain cmake/gcc-arm-none-eabi.cmake \
      -DVA_TRANSPORT=RAM_BUFFER
cmake --build build/rambuf -j
# or -DVA_TRANSPORT=JLINK_RTT into build/rtt

JLinkExe -NoGui 1 -CommanderScript flash.jlink   # device STM32F767ZI, si SWD,
                                                 # speed 25000, loadfile, r, g
```

Tunables: `-DTP_TARGET_KBPS=<n>` (KiB/s offered, 0 = unbounded).

## Measured baseline (this board: F767ZI, J-Link Compact Base @ 25 MHz SWD)

Saturation runs (offered 8000 KiB/s), 32-bit block reads of a running target:

| Drain path | Sustained | Notes |
|---|---|---|
| **ViewAlyzer app, `jlink-rambuf` transport** (JLinkDllProbe) | **~663 KB/s end-to-end** (1.36M pts/20 s, 0 corrupt) | the shipping path: SEGGER library loaded at runtime, target never halted |
| J-Link DLL raw bench (`JLINKARM_ReadMemEx`) | ~711 KB/s raw, ~698 KB/s drain cycle | upper bound of the above |
| **SEGGER RTT** (their own DLL RTT engine, ch1, 8 KB buffer) | **~687 KB/s** | SEGGER's native protocol on SEGGER's hardware |
| probe-rs 0.31 (J-Link driver, 15 MHz max in its speed table) | ~650 KB/s | sidecar-option ceiling |
| OpenOCD (jlink driver) TCL width-32 reads | ~610 KB/s | historical calibration point (the app's OpenOCD fallback was removed 2026-08) |

Takeaways:

- **The RAM-buffer ring drained via the J-Link DLL matches SEGGER's own RTT**
  (~700 vs ~687 KB/s) — RTT has no protocol magic, it is the same SWD block
  reads. One probe-agnostic firmware transport loses nothing on J-Link.
- All four paths land within ~15% of each other: at 25 MHz SWD the wire and
  the probe dominate, not host software. (Contrast ST-LINK V3, where the
  probe firmware's ~6 µs/word tax caps reads at ~640 KB/s.)
- These are **Compact Base** numbers — SEGGER quotes 0.5 MB/s for regular
  models and up to 2 MB/s for a Pro @ 50 MHz SWD, so a Pro should scale all
  of the above roughly 2x.
- For comparison, ARM's SDS-Framework measures its J-Link Pro RTT path at
  800 kB/s and ST-Link RTT at 130 kB/s.

## SEGGER RTT gotchas found while building this (JLINK_RTT transport)

1. **Channel 0 cannot be resized at runtime**: `SEGGER_RTT_ConfigUpBuffer(0,…)`
   silently keeps the compile-time 1 KB terminal buffer. The recorder's RTT
   buffer only takes effect on channels ≥ 1 — hence `VA_RTT_CHANNEL=1` here
   (which also matches the ViewAlyzer app's default RTT channel).
2. **Blocking RTT mode can starve interrupts**: recorder emissions run inside
   a critical section, so `BLOCK_IF_FIFO_FULL` + a saturating producer stalls
   SysTick (this benchmark's pacing clock froze at 1 ms). This project uses
   `SEGGER_RTT_MODE_NO_BLOCK_SKIP` — whole-packet drops, stream stays
   parseable, same semantics as the RAM buffer's DROP mode.
3. **The J-Link RTT auto-search can miss the control block** depending on
   where `.bss` places it; `nucleo_f767_jlink_rtt.vacf` pins it via
   `rtt-address` (the `_SEGGER_RTT` symbol in `build/rtt/*.map` — update it
   after code changes that move `.bss`).

## Record with the ViewAlyzer app

RAM-buffer build (the fast path):

```sh
ViewAlyzer --headless --config nucleo_f767_jlink_rambuf.vacf \
           --output f767.vadb --duration 20
```

The `jlink-rambuf` transport loads SEGGER's J-Link library at runtime (from
the `jlink` path, the standard SEGGER install dirs, or the system loader —
Linux/Windows/macOS alike) and attaches without reset or halt; if the library
is missing the capture fails with a clear error (install the SEGGER J-Link
software). RTT build: `nucleo_f767_jlink_rtt.vacf` (note the pinned
`rtt-address`).
