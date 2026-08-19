# Nucleo-F103RB - ViewAlyzer Bare-metal Example

Bare-metal (no RTOS) integration reference for the Nucleo-F103RB
(STM32F103RB, Cortex-M3 @ 72 MHz): user value traces, event spans, and ISR
instrumentation over the recorder core (`ViewAlyzerRecorder/core/ViewAlyzer.c`
from the sibling ViewAlyzer checkout - see the [top-level README](../../README.md)).

The firmware runs a layered integer DSP chain every loop pass (sensor ->
filter -> envelope -> classify -> stats), so a trace snapshot always lands in
real code with real call depth, and logs decimated user traces on top: two
graphs, a toggle, a bar, a periodic log string, and a `Work Block` event span.

Transports (CMake switch, one build dir per transport):

- **RAM buffer** (default) - the recorder owns a RAM ring the ViewAlyzer app
  drains through the on-board ST-LINK V2-1 with non-intrusive memory reads. No
  SWO pin, no wiring, and post-mortem snapshot capture works.
- **SWO / ITM** (`--transport swo`) - the classic trace pin.

## Build & flash

```sh
python3 build.py --flash                  # RAM buffer (default)
python3 build.py --transport swo --flash  # SWO / ITM
# --serial <probe-serial> picks a probe when several are connected
```

## Record

Open one of the `.vacf` connection configs in the ViewAlyzer app, or:

```sh
ViewAlyzer --headless --config nucleo_f103_rambuf.vacf \
           --output demo.vadb --duration 10 --verbose
```

For the transport bandwidth ceilings of this board, see the benchmark sibling
[`throughput/Nucleo_F103_Throughput`](../../throughput/Nucleo_F103_Throughput/).
