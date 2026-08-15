# Nucleo-F103RB - Transport Throughput Benchmark

Bare-metal (no RTOS) benchmark that measures **how many trace bytes per second
a ViewAlyzer transport can actually sustain** from a Nucleo-F103RB
(STM32F103RB, Cortex-M3 @ 72 MHz) to the host, over the board's on-board
ST-LINK V2-1. The same project builds for two transports via a CMake switch:

- **RAM buffer** (default) - the recorder owns a RAM ring the ViewAlyzer app
  drains through the ST-LINK with non-intrusive memory reads. No SWO pin, no
  wiring.
- **SWO / ITM** (`--transport swo`) - the classic trace pin.

How the paced saturation loop and the `_VA_TP` offered/dropped accounting
work is described in the [folder README](../README.md).

Two board realities to keep in mind: the probe is an **ST-LINK V2-1**
(distinctly slower than a V3 at both SWD memory reads and SWO), and the
F103's 20 KB SRAM means the recorder's default 8 KB RAM ring is a large
slice of the part - both are representative for this class of MCU.

## Build & flash

```sh
python3 build.py --flash                  # RAM buffer (default)
python3 build.py --transport swo --flash  # SWO / ITM
# --serial <probe-serial> picks a probe when several are connected
```

(Or drive CMake directly - see the build.py header. Tunables:
`-DTP_TARGET_KBPS=<n>` offered load in KiB/s, 0 = unbounded blast.)

## Record & measure

```sh
# RAM buffer
ViewAlyzer --headless --config nucleo_f103_rambuf.vacf \
           --output tp_rambuf.vadb --duration 20 --verbose
# SWO / ITM
ViewAlyzer --headless --config nucleo_f103_swo.vacf \
           --output tp_swo.vadb --duration 20 --verbose
```

While recording, the GUI shows the live throughput under the record button;
the finished recording's **Overview → Trace Summary** shows Avg / Peak
throughput.

## Measured baseline

See [../RESULTS.md](../RESULTS.md) for this board's measured numbers
alongside the other cores.
