# Bare-metal Example Projects

Bare-metal projects (no RTOS) live here. They use only the recorder core
(`ViewAlyzerRecorder/core/ViewAlyzer.c` from the ViewAlyzer repo, resolved as
described in the [top-level README](../README.md)) and demonstrate user
traces, events, and ISR instrumentation.

| Project | Board | Transport |
|---------|-------|-----------|
| [Nucleo_F103_VA](Nucleo_F103_VA/) | Nucleo-F103RB (STM32F103, Cortex-M3 @ 72 MHz) | **RAM buffer** via the on-board ST-LINK V2-1 (switchable to ITM/SWO) |
| [Nucleo_G474_VA](Nucleo_G474_VA/) | Nucleo-G474RE (STM32G474, Cortex-M4F @ 170 MHz) | **RAM buffer** via the on-board ST-LINK V3 (switchable to ITM/SWO) |
| [Nucleo_H723_VA](Nucleo_H723_VA/) | Nucleo-H723ZG (STM32H723, Cortex-M7 @ 192 MHz) | **RAM buffer** via the on-board ST-Link (switchable to ITM/SWO) |

Every project builds and flashes the same way:

```sh
python3 build.py --flash                  # RAM buffer (default)
python3 build.py --transport swo --flash  # ITM / SWO variant
# --serial <probe-serial> picks a probe when several are connected
```

The transport *bandwidth benchmarks* that used to live here moved to
[`../throughput/`](../throughput/) - those deliberately flood the pipe and
are not integration references.

See the recorder core README (`ViewAlyzerRecorder/core/README.md` in the
ViewAlyzer repo) for the bare-metal API and a minimal startup example.
