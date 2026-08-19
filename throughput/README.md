# Transport Throughput Benchmarks

Bare-metal projects that measure **how many trace bytes per second a
ViewAlyzer transport can actually sustain** from a target to the host. One
project per board, together covering the common Cortex-M cores:

| Project | Board | Core | Clock | Probe | Transports |
|---------|-------|------|-------|-------|------------|
| [Nucleo_F103_Throughput](Nucleo_F103_Throughput/) | Nucleo-F103RB | Cortex-M3 | 72 MHz | ST-LINK V2-1 (on-board) | RAM buffer, SWO |
| [Nucleo_G474_Throughput](Nucleo_G474_Throughput/) | Nucleo-G474RE | Cortex-M4F | 170 MHz | ST-LINK V3 (on-board) | RAM buffer, SWO |
| [Nucleo_H503_Throughput](Nucleo_H503_Throughput/) | Nucleo-H503RB | Cortex-M33 | 240 MHz | ST-LINK V3 (on-board) | RAM buffer, SWO |
| [Nucleo_U385_Throughput](Nucleo_U385_Throughput/) | Nucleo-U385RG-Q | Cortex-M33 | 96 MHz | ST-LINK V3 (on-board) | RAM buffer, SWO |
| [Nucleo_F767_Throughput](Nucleo_F767_Throughput/) | Nucleo-F767ZI | Cortex-M7 | 96 MHz | SEGGER J-Link (external) | RAM buffer, RTT |
| [Nucleo_H723_Throughput](Nucleo_H723_Throughput/) | Nucleo-H723ZG | Cortex-M7 | 192 MHz | ST-LINK V3 (on-board) | RAM buffer, SWO |

These are *benchmarks*, not integration references - they deliberately
saturate the transport. For normal usage examples see
[`../baremetal/`](../baremetal/), [`../freertos/`](../freertos/), and
[`../zephyr/`](../zephyr/).

## How to interpret these numbers

**Saturation is the benchmark's job, not something real firmware does.**
Reaching these ceilings requires emitting trace packets in a tight loop and
doing nothing else. A real application - even a densely instrumented RTOS
with dozens of tasks, queues, and ISRs - generates a few thousand events per
second, an order of magnitude below the RAM-buffer ceiling, so in normal use
every transport is lossless *and* non-intrusive at the same time. The
lossy/lossless distinction below only tells you which property each
transport preserves in the pathological case, and how much headroom you
have before it matters.

## How the benchmark works

The main loop emits fixed-size trace packets as fast as a **paced offered
load** allows (`-DTP_TARGET_KBPS=<n>`, default 8000 KiB/s - far above any
debug-probe transport ceiling). The pipe therefore *saturates*, and three
numbers tell the story:

| Number | Where it comes from |
|---|---|
| **Delivered KB/s** | the ViewAlyzer app's live throughput readout (bytes the host actually received) |
| **Offered / dropped bytes** | the recorder's `_VA_TP` counter block, enabled by `-DVA_TP_TEST=1` |
| **Dropped packets** | the RAM-buffer control block's `droppedPackets` (RAM buffer only) |

`delivered = offered − dropped`. The two transport families behave
differently at saturation:

- **RAM buffer / RTT** are *non-intrusive*: the CPU never blocks; excess
  load is shed as whole dropped packets.
- **SWO / ITM** is *lossless by back-pressure*: the ITM FIFO spins the CPU,
  so the firmware self-throttles to the line rate.

## Build, flash, measure

Every project has the same three steps (see its README for specifics):

```sh
python3 build.py --flash                  # default transport (RAM buffer)
python3 build.py --transport swo --flash  # or: rtt on the J-Link project
# --serial <probe-serial> picks a probe when several are connected

ViewAlyzer --headless --config <board>_rambuf.vacf --output tp.vadb --duration 20 --verbose
```

While recording, the GUI shows the live throughput under the record button;
afterwards the recording's **Overview → Trace Summary** shows Avg / Peak
throughput under *Stream Integrity* (the same numbers are stored in the
`.vadb` and reachable from the headless CLI's query modes).

## Measured baselines

See [RESULTS.md](RESULTS.md) for the measured numbers across all six boards
(and the per-project READMEs for board-specific notes).
