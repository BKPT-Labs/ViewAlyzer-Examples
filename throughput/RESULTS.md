# Measured Transport Throughput

What each ViewAlyzer transport sustains from target to host, measured with
the projects in this folder across the common Cortex-M cores. 20-second
saturation runs (offered load 8000 KiB/s - far above every ceiling), captured
with the ViewAlyzer headless CLI; **sustained** is total delivered trace
bytes over the capture span, **peak** is the capture's peak-throughput
metric. All runs delivered **zero corrupt bytes**.

## How to read these numbers

**These are stress ceilings, not everyday behavior.** Saturating a transport
takes firmware emitting trace packets in a tight loop and doing nothing
else - no real application does that. A densely instrumented RTOS app with
dozens of tasks, queues, and ISRs generates a few thousand events per
second, an order of magnitude under the RAM-buffer ceiling, so **in normal
use every transport is simultaneously lossless and non-intrusive** and every
capture is complete. The "at saturation" column only says which property
each transport is engineered to preserve if a pathological burst ever
exceeded the ceiling - and how much headroom you have before that could
matter.

| Board | MCU | Core | Clock | Probe | Transport | Sustained | Peak | Events/s | At saturation |
|-------|-----|------|-------|-------|-----------|----------:|-----:|---------:|---------------|
| Nucleo-F103RB | STM32F103RB | Cortex-M3 | 72 MHz | ST-LINK V2-1 (on-board) | RAM buffer | **155 KiB/s** | 156 KiB/s | 14.4 k | drops whole packets, CPU never blocks |
| Nucleo-F103RB | STM32F103RB | Cortex-M3 | 72 MHz | ST-LINK V2-1 (on-board) | SWO 2 MHz | **143 KiB/s** | 195 KiB/s | 13.0 k | lossless - CPU self-throttles |
| Nucleo-G474RE | STM32G474RE | Cortex-M4F | 170 MHz | ST-LINK V3 (on-board) | RAM buffer | **545 KiB/s** | 549 KiB/s | 50.6 k | drops whole packets, CPU never blocks |
| Nucleo-G474RE | STM32G474RE | Cortex-M4F | 170 MHz | ST-LINK V3 (on-board) | SWO 2 MHz | **143 KiB/s** | 196 KiB/s | 13.0 k | lossless - CPU self-throttles |
| Nucleo-H503RB | STM32H503RB | Cortex-M33 | 240 MHz | ST-LINK V3 (on-board) | SWO 2 MHz | **143 KiB/s** | 195 KiB/s | 13.0 k | lossless - CPU self-throttles |
| Nucleo-U385RG-Q | STM32U385RG | Cortex-M33 | 96 MHz | ST-LINK V3 (on-board) | RAM buffer | **537 KiB/s** | 543 KiB/s | 49.6 k | drops whole packets, CPU never blocks |
| Nucleo-U385RG-Q | STM32U385RG | Cortex-M33 | 96 MHz | ST-LINK V3 (on-board) | SWO 2 MHz | **143 KiB/s** | 197 KiB/s | 13.0 k | lossless - CPU self-throttles |
| Nucleo-F767ZI | STM32F767ZI | Cortex-M7 | 96 MHz | J-Link Compact Base (external) | RAM buffer | **580 KiB/s** | 602 KiB/s | 53.9 k | drops whole packets, CPU never blocks |
| Nucleo-F767ZI | STM32F767ZI | Cortex-M7 | 96 MHz | J-Link Compact Base (external) | SEGGER RTT | **664 KiB/s** | 701 KiB/s | 61.7 k | drops whole packets (`NO_BLOCK_SKIP`) |
| Nucleo-H723ZG | STM32H723ZG | Cortex-M7 | 192 MHz | ST-LINK V3 (on-board) | RAM buffer | **~588 KiB/s** ① | | | drops whole packets, CPU never blocks |
| Nucleo-H723ZG | STM32H723ZG | Cortex-M7 | 192 MHz | ST-LINK V3 (on-board) | SWO 2 MHz | **~196 KiB/s** ① | | | lossless - CPU self-throttles |

SWO on a J-Link is not currently supported by the app, so the F767 has no
SWO row.

## What the numbers say

- **The probe generation dominates RAM-buffer throughput, not the MCU.**
  ST-LINK V3 and J-Link sustain 537-664 KiB/s regardless of core or clock -
  a 96 MHz Cortex-M33 and a 170 MHz Cortex-M4 land within 2% of each other -
  while the older ST-LINK V2-1 manages 155 KiB/s on the same firmware. The
  debug probe's memory-read rate is the pipe.
- **SEGGER RTT on a J-Link is the fastest transport measured** (664 KiB/s
  sustained), with the RAM buffer close behind - and the RAM buffer needs no
  SEGGER stack and works on every probe.
- **SWO is the line rate, every time.** At 2 MHz UART-mode SWO the ceiling is
  195.3 KiB/s (2 Mbit/s ÷ 10 bits/byte); every board peaks exactly there and
  sustains ~143 KiB/s under continuous saturation, identical across M3, M4,
  and M33 - the line and the host drain pattern set the number, the MCU is
  irrelevant. Unlike the RAM ring it never drops: the ITM FIFO back-pressures
  the CPU instead.
- **Events per second** is often the more intuitive unit: at a typical
  ~11-byte packet, ~50 k events/s stream through a V3 RAM buffer and ~13 k
  events/s through 2 MHz SWO - decimate your logging accordingly.

Notes:

① The H723 rows are the baseline from
  [Nucleo_H723_Throughput/README.md](Nucleo_H723_Throughput/README.md),
  measured on the same app version's direct ST-Link driver; that board was
  not on the bench for this run.

## Method / reproducibility

- Firmware: the per-board projects in this folder, Release builds, paced
  offered load `TP_TARGET_KBPS=8000` (KiB/s), i.e. saturation. Recorder
  ring: 8 KiB, default DROP mode.
- Host: `ViewAlyzer --headless --config <board>_<transport>.vacf
  --output tp.vadb --duration 20 --verbose`, one board active at a time -
  every other board on the bench was held in erased flash so nothing else
  produced debug-port traffic.
- Bench: macOS (Apple Silicon), probes behind a USB-C hub + USB 2.0 hub.
  The hub costs real bandwidth on the fast transports: the same F767 + J-Link
  measured directly on a root port reaches ~663 KiB/s (RAM buffer) and
  ~687 KiB/s (RTT) - see
  [Nucleo_F767_Throughput/README.md](Nucleo_F767_Throughput/README.md) -
  vs. 580 / 664 KiB/s here, so budget ~10% for a hub.
- Integrity checked per run in the resulting `.vadb`: `corrupt_bytes = 0`,
  and the sequence-number accounting (`lost_events`, `seq_gaps`) records
  exactly what saturation shed. A run that "succeeds" with 0 events is a
  connection problem, not a fast one - always check the event count.
