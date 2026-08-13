# Nucleo-H723 — Transport Throughput Benchmark

Bare-metal (no RTOS) benchmark that measures **how many trace bytes per second
a ViewAlyzer transport can actually sustain** from a Nucleo-H723ZG (STM32H723ZG,
Cortex-M7 @ 192 MHz) to the host. The same project builds for two transports via
a CMake switch:

- **RAM buffer** (default) — the recorder owns a RAM ring the ViewAlyzer app
  drains through the on-board **ST-Link** with non-intrusive memory reads. No
  SWO pin, no wiring.
- **SWO / ITM** (`-DVA_TRANSPORT=ARM_ITM`) — the classic trace pin.

## How it works

The main loop emits fixed-size trace packets as fast as a **paced offered load**
allows (`TP_TARGET_KBPS`, default 8000 KiB/s — far above any real transport
ceiling). The pipe therefore *saturates*, and three numbers tell the story:

| Number | Where it comes from |
|---|---|
| **Delivered KB/s** | the ViewAlyzer app's live throughput readout (bytes the host actually received) |
| **Offered / dropped bytes** | the recorder's `_VA_TP` counter block, enabled by `-DVA_TP_TEST=1` |
| **Dropped packets** | the RAM-buffer control block's `droppedPackets` (RAM buffer only) |

`delivered = offered − dropped`. `_VA_TP` is a compile-gated struct
(magic `"VATPCNT1"`) the debugger reads by symbol; it costs nothing in a normal
build (the counters and symbol vanish when `VA_TP_TEST` is undefined).

## Build & flash

```sh
# RAM buffer (default)
cmake -S . -B build/rambuf -G Ninja --toolchain cmake/gcc-arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DVA_TRANSPORT=RAM_BUFFER
cmake --build build/rambuf -j

# SWO / ITM
cmake -S . -B build/swo -G Ninja --toolchain cmake/gcc-arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DVA_TRANSPORT=ARM_ITM
cmake --build build/swo -j

# flash (OpenOCD — note the -s scripts path so gdb_helper.tcl resolves)
openocd -s /usr/local/share/openocd/scripts \
        -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "program build/rambuf/Nucleo_H723_Throughput.elf verify reset exit"
```

Tunables (CMake cache vars): `-DVA_TRANSPORT=…`, `-DTP_TARGET_KBPS=<n>` (0 =
unbounded blast — note the 32-bit offered counter wraps in seconds when
unbounded, so prefer a bounded rate for clean accounting).

## Record & measure

```sh
# RAM buffer
ViewAlyzer --headless --config nucleo_h723_rambuf.vacf \
           --output tp_rambuf.vadb --duration 20 --verbose
# SWO / ITM
ViewAlyzer --headless --config nucleo_h723_swo.vacf \
           --output tp_swo.vadb --duration 20 --verbose
```

The `--verbose` log prints `Throughput: X KB/s` every ~500 ms. In the GUI, the
same number appears live under the record button, and the recording's **Overview
→ Trace Summary** shows **Avg / Peak Throughput** under *Stream Integrity*.

Read the firmware counters directly with the probe (symbol addresses are in the
`.map`; `_VA_TP` layout is `offeredPackets, offeredBytes, droppedPackets,
droppedBytes` after the 8-byte magic):

```sh
openocd -s /usr/local/share/openocd/scripts -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "init; halt" -c "mdw <_VA_TP addr> 6" -c "resume; exit"
```

## Measured baseline (this board: Nucleo-H723ZG @ 192 MHz, STLINK-V3, OpenOCD)

20 s runs; saturation = offered load 8000 KiB/s:

| Transport | Config | Sustained delivered | Sustained lossless | Character |
|---|---|---|---|---|
| **RAM buffer, direct USB driver** | ST-Link V3 @ 24 MHz, in-app libusb driver, 8 KB ring | **~588 KB/s** (saturation drain ceiling) | **≥512 KB/s** (0 drops, 0 corrupt over 20 s) | *Non-intrusive* — the CPU never blocks; excess load is dropped whole packets |
| RAM buffer, OpenOCD fallback (historical - fallback removed 2026-08) | ST-Link V3 @ 24 MHz SWD, width-32 TCL reads, 8 KB ring | ~494 KB/s | >=410 KB/s | same; kept for calibration only |
| **SWO / ITM** | 2 MHz SWO, UART mode | **~196 KB/s** | ~196 KB/s | *Lossless by back-pressure* — the ITM FIFO spins the CPU, so the firmware self-throttles to the line rate |

Notes:

- These RAM-buffer numbers require the 2026-07 ViewAlyzer drain path (direct
  libusb ST-Link driver). The original width-8 / 4 MHz
  drain measured **15.6 KB/s** — a 38x difference from host-side fixes alone;
  see `ViewAlyzer-App/docs/dev-notes/rambuf-throughput-research.md` for the
  full story. ~640 KB/s is the ST-LINK V3 (fw J15) probe-firmware ceiling for
  running-target reads; newer probe firmware may lift it.
- For calibration: ARM's SDS-Framework measures its ST-Link RTT path at
  130 kB/s and J-Link Pro RTT at 800 kB/s; a regular J-Link does ~500 kB/s.
- SWO ≈ 98% of the 2 Mbit/s ÷ 10-bits-per-byte UART-mode ceiling (195.3 KB/s)
  and scales ~linearly with SWO clock (STLINK-V3 traces up to 24 MHz —
  untested here) at the cost of CPU spin time under load.
- Bottom line: the **RAM buffer** is now the higher-throughput option on
  ST-Link *and* stays zero-CPU-overhead; choose **SWO** when you want hard
  losslessness under overload (back-pressure semantics) or a second
  concurrent stream.

## Notes

- Caches are left disabled by this template, so debugger reads are always
  coherent. If you enable the D-cache, place the RAM ring in a non-cacheable MPU
  region via `VA_RAMBUF_ATTRIBUTES`.
- This project is a *benchmark*, not a usage example — it deliberately floods
  the transport. For a normal integration reference see the sibling
  `Nucleo_H723_VA` project.
