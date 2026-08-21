# ViewAlyzer Example Projects

Standalone, self-contained example projects that demonstrate how to integrate
the **ViewAlyzer Recorder** into your firmware. Each subproject is intended to
be cloned (or copied) and built without needing any other configuration beyond
the standard toolchain for that target.

This repo doubles as integration documentation:

- **[AI_INTEGRATION.md](AI_INTEGRATION.md)** - the complete integration
  reference on one page (transports, per-RTOS recipes, `.vacf` configs,
  capture, troubleshooting). Written for humans *and* for pointing an AI
  assistant at ("integrate ViewAlyzer into my project, follow this guide").
- Each Zephyr/FreeRTOS/baremetal project is a working reference you can diff
  your own project against.

The projects are grouped by RTOS / runtime:

```
ViewAlyzer-Examples/
├── baremetal/           # No RTOS - direct user trace + ISR instrumentation
├── freertos/            # FreeRTOS-based examples (task switches, sync objects)
├── zephyr/              # Zephyr RTOS examples using the ViewAlyzer Zephyr module
├── throughput/          # Transport bandwidth benchmarks (one board per Cortex-M core)
├── Desktop-CPP-UDP/     # Host-side C++ example that streams traces over UDP
└── Desktop-Python-UDP/  # Host-side Python UDP sender package + examples
```

## The Recorder

All firmware projects pull recorder sources from the `ViewAlyzerRecorder`
directory that ships in the [ViewAlyzer](https://github.com/BKPT-Labs/ViewAlyzer)
repository. Each project resolves it in this order:

1. an explicit CMake option (`-DVIEWALYZER_MODULE_PATH=...` for Zephyr
   projects, `-DVIEWALYZER_RECORDER_DIR=...` for the others)
2. the same name as an environment variable
3. a `ViewAlyzer` checkout sitting next to this repository

So the zero-configuration setup is two sibling clones:

```
<your-workspace>/
├── ViewAlyzer/            # contains ViewAlyzerRecorder/
└── ViewAlyzer-Examples/   # this repository
```

## Transports - which one to use

The recorder can stream over several transports; pick per board:

- **RAM buffer** (*recommended*) - the recorder writes a ring buffer in
  target RAM and the host drains it through the debug probe with plain
  memory reads. Works with the on-board ST-LINK every Nucleo ships with, no
  SWO pin, no SEGGER stack, best sustained throughput in practice, and it
  enables post-mortem **snapshot** capture (run untethered, read the last
  trace window out later).
- **ITM/SWO** - classic ARM trace over the SWO pin (ST-LINK).
- **SEGGER RTT** - when the board's probe runs J-Link firmware.
- **UDP** - desktop/simulation producers (see the Desktop examples).

## Connection configs (`.vacf`)

Every firmware example ships one or more `.vacf` files - small JSON
**connection configs** (transport, target device, probe speed, SWO
frequency, …). Open one in the ViewAlyzer app and the connection is set up
in one click, or pass it on the CLI:

```bash
ViewAlyzer --headless --config nucleo_g474_zephyr_swo.vacf --output run.vadb --duration 10
```

Commit one per board next to your own firmware so nobody re-enters probe
settings ever again. Key reference: [AI_INTEGRATION.md](AI_INTEGRATION.md#step-3--connect-and-capture).

## Machine-specific tool paths

Committed files never contain machine paths. Anything local - Zephyr
checkout, toolchain, OpenOCD, J-Link - goes into gitignored config files
read by the build scripts: a `tools.local[.win|.linux|.mac].json` at this
repo root (shared by all projects), overridable per project with
`build.local[.os].json` next to its `build.py`. Copy
[`tools.local.example.json`](tools.local.example.json) and keep only the
keys auto-detection gets wrong. The per-OS variants exist so a dual-boot
machine can share a single checkout.

---

## zephyr/

Zephyr projects use the recorder as an external Zephyr module - no sources
are copied. **[STM32-Zephyr-VA-Demo](zephyr/STM32-Zephyr-VA-Demo)** is the
place to start: a multi-board, multi-transport demo that exercises every
traceable kernel object, with a quickstart and a step-by-step
[TUTORIAL](zephyr/STM32-Zephyr-VA-Demo/TUTORIAL.md) for adding ViewAlyzer to
your own app. Three small Cortex-M0+ projects (G0B1RE, C031C6, L031K6) are
porting references for constrained parts, and the MAX32657 project covers an
ADI Cortex-M33 over RTT. See [zephyr/README.md](zephyr/README.md) for the
full table and the expected Zephyr workspace layout.

```bash
cd zephyr/STM32-Zephyr-VA-Demo
python3 build.py            # build for the default board (Nucleo-G474RE)
python3 build.py flash g4   # build + flash via the onboard ST-LINK
```

## freertos/

FreeRTOS projects pull in the `core/` recorder plus the FreeRTOS adapter
(`freertos/VA_Adapter_FreeRTOS.c`) and set `VA_RTOS_SELECT=1` so task
switches, queues, semaphores, and mutexes are captured automatically.

Note the defines live in `add_definitions(...)` at the top of each project's
`CMakeLists.txt`, which is directory-scoped: the FreeRTOS kernel target gets
the identical set. That matters - the kernel compiles the trace hooks, and a
kernel built with a different `VA_TRACE_*` set installs different hooks.

| Project | Board | Notes |
|---------|-------|-------|
| [Nucleo_F103_VA](freertos/Nucleo_F103_VA) | Nucleo-F103RB | STM32CubeMX + CMake, FreeRTOS |
| [Nucleo_F446RE](freertos/Nucleo_F446RE) | Nucleo-F446RE | STM32CubeMX + CMake, FreeRTOS, SEGGER RTT |
| [Nucleo_G474_VA](freertos/Nucleo_G474_VA) | Nucleo-G474RE | STM32CubeMX + CMake, FreeRTOS |
| [Nucleo_U385](freertos/Nucleo_U385) | Nucleo-U385 | STM32CubeMX + CMake, FreeRTOS (kernel v10.6, `ViewAlyzerFreeRTOSHook_V10_4_Plus.h`) |

Build (every project ships a `build.py`; plain CMake works too):

```bash
cd freertos/Nucleo_G474_VA
python3 build.py                    # build
python3 build.py --flash            # build + flash via the on-board ST-LINK
python3 build.py --flash --serial <probe-serial>   # pick one of several probes
```

## baremetal/

Bare-metal projects use only `core/ViewAlyzer.c` (and `viewalyzer_cobs.c` if a
framed custom transport is needed). They demonstrate user traces, events, and
ISR instrumentation without any RTOS hooks.

| Project | Board | Highlights |
|---------|-------|-----------|
| [Nucleo_F103_VA](baremetal/Nucleo_F103_VA) | Nucleo-F103RB (STM32F103RB, Cortex-M3) | Small-SRAM reference - RAM buffer through the on-board ST-LINK V2-1, switchable to ITM/SWO |
| [Nucleo_C031_VA](baremetal/Nucleo_C031_VA) | Nucleo-C031C6 (STM32C031C6, Cortex-M0+) | Smallest-core reference - CUSTOM_TIMER timestamps (no DWT cycle counter), RAM buffer (no ITM), live PC-sample profile by DWT_PCSR polling |
| [Nucleo_C071_VA](baremetal/Nucleo_C071_VA) | Nucleo-C071RB (STM32C071RB, Cortex-M0+) | Trace Domain reference - synthetic 6-channel IMU whose `imu.*` traces pair with the `com.bkpt.demo.imu` `.vadomain` descriptor in ViewAlyzer (units, groups, angle/gauge display overrides) |
| [Nucleo_G474_VA](baremetal/Nucleo_G474_VA) | Nucleo-G474RE (STM32G474RE, Cortex-M4F) | RAM buffer through the on-board ST-LINK V3, switchable to ITM/SWO |
| [Nucleo_H723_VA](baremetal/Nucleo_H723_VA) | Nucleo-H723ZG (STM32H723ZG, Cortex-M7) | **RAM buffer transport** - streams through the on-board ST-Link with no SWO pin and no SEGGER RTT; switchable to ITM/SWO with `-DVA_TRANSPORT=ARM_ITM` |

## throughput/

Transport bandwidth benchmarks - one board per Cortex-M core (M3, M4, M33,
M7), each measuring what RAM-buffer / SWO / RTT capture actually sustains
from that target. These deliberately saturate the pipe and are **not**
integration references. See [throughput/README.md](throughput/README.md) for
how the benchmark works and [throughput/RESULTS.md](throughput/RESULTS.md)
for measured numbers.

## Desktop-CPP-UDP/

Host-side C++ example that streams traces over UDP. It bundles the
ViewAlyzer C UDP sender library in its `c-udp/` subdirectory. Useful for
desktop simulation, prototyping, or producing traces from a non-embedded
source. Build with any standard C++17 toolchain:

```bash
cd Desktop-CPP-UDP
cmake -S . -B build
cmake --build build
```

## Desktop-Python-UDP/

Python package for sending ViewAlyzer trace data over UDP (stdlib only,
Python 3.8+), with runnable examples:

```bash
pip install ./Desktop-Python-UDP
python Desktop-Python-UDP/examples/basic_example.py
```

---

## License

See [LICENSE](LICENSE).
