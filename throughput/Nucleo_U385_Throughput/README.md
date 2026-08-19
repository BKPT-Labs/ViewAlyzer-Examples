# Nucleo-U385RG-Q - Transport Throughput Benchmark

Bare-metal (no RTOS) benchmark that measures **how many trace bytes per second
a ViewAlyzer transport can actually sustain** from a Nucleo-U385RG-Q
(STM32U385RG-Q, Cortex-M33 @ 96 MHz) to the host, over the board's on-board
ST-LINK V3. The same project builds for two transports via a CMake switch:

- **RAM buffer** (default) - the recorder owns a RAM ring the ViewAlyzer app
  drains through the ST-LINK with non-intrusive memory reads. No SWO pin, no
  wiring.
- **SWO / ITM** (`--transport swo`) - the classic trace pin.

How the paced saturation loop and the `_VA_TP` offered/dropped accounting
work is described in the [folder README](../README.md).

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
ViewAlyzer --headless --config nucleo_u385_rambuf.vacf \
           --output tp_rambuf.vadb --duration 20 --verbose
# SWO / ITM
ViewAlyzer --headless --config nucleo_u385_swo.vacf \
           --output tp_swo.vadb --duration 20 --verbose
```

While recording, the GUI shows the live throughput under the record button;
the finished recording's **Overview → Trace Summary** shows Avg / Peak
throughput.

## Measured baseline

See [../RESULTS.md](../RESULTS.md) for this board's measured numbers
alongside the other cores.
