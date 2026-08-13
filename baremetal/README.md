# Bare-metal Example Projects

Bare-metal projects (no RTOS) live here. They use only the recorder core
(`ViewAlyzerRecorder/core/ViewAlyzer.c` from the ViewAlyzer repo, resolved as
described in the [top-level README](../README.md)) and demonstrate user
traces, events, and ISR instrumentation.

| Project | Board | Transport |
|---------|-------|-----------|
| [Nucleo_H723_VA](Nucleo_H723_VA/) | Nucleo-H723ZG (STM32H723, Cortex-M7 @ 192 MHz) | **RAM buffer** via the on-board ST-Link (switchable to ITM/SWO) |

See the recorder core README (`ViewAlyzerRecorder/core/README.md` in the
ViewAlyzer repo) for the bare-metal API and a minimal startup example.
