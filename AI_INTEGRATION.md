# ViewAlyzer Integration Guide

A complete, self-contained reference for integrating the **ViewAlyzer
Recorder** into a firmware project and capturing traces with the ViewAlyzer
app. Written to be equally useful to a human following along and to an AI
assistant doing the integration for them — everything needed is on this one
page, with working reference projects in this repository.

## The pieces

| Piece | What it is |
|-------|-----------|
| **ViewAlyzer Recorder** | Small C firmware library (`ViewAlyzerRecorder/` in the [ViewAlyzer repo](https://github.com/BKPT-Labs/ViewAlyzer)). Compiles into your firmware; emits trace events (threads, ISRs, sync objects, user values) over a transport. |
| **ViewAlyzer app** | Desktop host (GUI and `--headless` CLI in one binary). Connects through a debug probe or UDP, decodes the stream live, records to `.vadb`. |
| **`.vacf` file** | JSON **connection config**: transport + probe/target parameters for one board setup. Load it in the app (or pass `--config` on the CLI) instead of re-entering settings every session. Commit one per board next to your firmware. |
| **`.vadb` file** | A recording — a SQLite database of decoded events (directly queryable; raw wire bytes preserved in `raw_log`). |

## Step 0 — Get the recorder

```bash
git clone https://github.com/BKPT-Labs/ViewAlyzer.git
```

The examples in this repo find it automatically when the two repos are
cloned side by side; in your own project you point your build at
`ViewAlyzer/ViewAlyzerRecorder` explicitly (shown per-RTOS below). No
package manager, no library build step — you compile a few `.c` files.

## Step 1 — Pick a transport

| Transport | Probe needed | Extra wiring | Notes |
|-----------|--------------|--------------|-------|
| **RAM buffer** (`varambuf`) — **recommended** | Any SWD probe, including the on-board ST-LINK every Nucleo ships with | none | The recorder writes into a ring buffer in target RAM; the host drains it via probe memory reads. Best sustained throughput in practice, no SWO pin, no SEGGER stack, and it also enables **snapshot mode** (run untethered, read the last trace window out post-mortem). |
| ITM / SWO | ST-LINK (or any SWO-capable probe) | SWO pin routed | Classic ARM trace path. Host and target SWO frequency must match. |
| SEGGER RTT | J-Link (or J-Link OB) | none | Good when the board already has J-Link firmware on its probe. |
| UDP | none (network) | none | For desktop/simulation producers — see the `Desktop-CPP-UDP` and `Desktop-Python-UDP` examples. |

Cortex-M0/M0+/M23 parts have no DWT cycle counter: they additionally need a
hardware timer as the timestamp source (`CUSTOM_TIMER`). Working references:
`zephyr/Nucleo-G0B1RE-*` (32-bit timer), `Nucleo-C031C6-*` and
`Nucleo-L031K6-*` (16-bit timer, single-digit-KB RAM).

## Step 2 — Integrate (per RTOS)

### Zephyr

The recorder is a proper Zephyr module. Reference:
[`zephyr/STM32-Zephyr-VA-Demo`](zephyr/STM32-Zephyr-VA-Demo) (its
[TUTORIAL.md](zephyr/STM32-Zephyr-VA-Demo/TUTORIAL.md) walks this step by
step).

`CMakeLists.txt` — before `find_package(Zephyr ...)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES /path/to/ViewAlyzer/ViewAlyzerRecorder)
```

`prj.conf`:

```conf
CONFIG_VIEWALYZER=y
CONFIG_TRACING_USER=y          # required — routes Zephyr's trace hooks to the module

# one transport:
CONFIG_VIEWALYZER_TRANSPORT_RAMBUF=y  # or ..._ITM=y (SWO) or ..._RTT=y (J-Link)

# categories you want (each is independent; off = compiled out, zero cost):
CONFIG_VIEWALYZER_TRACE_THREADS=y
CONFIG_VIEWALYZER_TRACE_ISRS=y
CONFIG_VIEWALYZER_TRACE_MUTEXES=y
CONFIG_VIEWALYZER_TRACE_SEMAPHORES=y
CONFIG_VIEWALYZER_TRACE_MESSAGE_QUEUES=y
CONFIG_VIEWALYZER_TRACE_SLEEP=y
CONFIG_VIEWALYZER_STACK_USAGE=y
```

`main.c`:

```c
#include "ViewAlyzer.h"
#include "VA_Adapter_Zephyr.h"

VA_Init(SystemCoreClock);                 /* after clocks are up            */
VA_Zephyr_RegisterExistingThreads();      /* announce threads created before init */

VA_RegisterUserTrace(1, "Counter", VA_USER_TYPE_COUNTER);
VA_LogTrace(1, counter);                  /* your own values, any thread    */
```

Kernel objects and thread switches are traced automatically by the adapter.
Gotchas: the module path must be appended *before* `find_package(Zephyr)`;
RTT needs the `segger` module fetched in your west workspace; `ViewAlyzer.h`
includes `main.h`, so a Zephyr app needs a small `main.h` shim with the MCU
device header (see the demo's `src/main.h`).

### FreeRTOS

Reference: [`freertos/Nucleo_G474_VA`](freertos/Nucleo_G474_VA) (CubeMX +
CMake). Compile the core, the adapter, and set the defines **directory-wide**
so the FreeRTOS kernel target gets the identical set (the kernel compiles the
trace hooks — a kernel built with different `VA_TRACE_*` settings installs
different hooks):

```cmake
add_definitions(-DVA_ENABLED=1 -DVA_RTOS_SELECT=1
                -DVA_DEVICE_HEADER=stm32g474xx.h
                -DVA_TRANSPORT=RAM_BUFFER -DVA_RAMBUF_SIZE=16384u)

set(VIEWALYZER_RECORDER_DIR /path/to/ViewAlyzer/ViewAlyzerRecorder)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c
    ${VIEWALYZER_RECORDER_DIR}/freertos/VA_Adapter_FreeRTOS.c)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${VIEWALYZER_RECORDER_DIR}/core
    ${VIEWALYZER_RECORDER_DIR}/freertos)

# the kernel must see the hook headers too (before its own includes) —
# the target is named `FreeRTOS` in the CubeMX-generated projects:
target_include_directories(FreeRTOS BEFORE PRIVATE
    ${VIEWALYZER_RECORDER_DIR}/core
    ${VIEWALYZER_RECORDER_DIR}/freertos)
```

Then `VA_Init(SystemCoreClock);` after HAL/clock init, and the FreeRTOS hook
header wired into `FreeRTOSConfig.h` (see the reference project; kernels
v10.4+ use `ViewAlyzerFreeRTOSHook_V10_4_Plus.h`).

### Bare metal (no RTOS)

Reference: [`baremetal/Nucleo_H723_VA`](baremetal/Nucleo_H723_VA). Only the
core file is needed:

```cmake
add_definitions(-DVA_ENABLED=1 -DVA_RTOS_SELECT=0
                -DVA_DEVICE_HEADER=stm32h723xx.h
                -DVA_TRANSPORT=RAM_BUFFER)   # RAM_BUFFER | ARM_ITM | JLINK_RTT | CUSTOM_TRANSPORT

target_sources(app PRIVATE ${VIEWALYZER_RECORDER_DIR}/core/ViewAlyzer.c)
target_include_directories(app PRIVATE ${VIEWALYZER_RECORDER_DIR}/core)
```

You get user traces, user events, strings, and ISR instrumentation; there are
no RTOS objects to trace.

### Desktop / non-embedded

Use the UDP sender libraries: [`Desktop-CPP-UDP`](Desktop-CPP-UDP) (C/C++)
or [`Desktop-Python-UDP`](Desktop-Python-UDP) (pure-stdlib Python package).
The app listens with `--transport udp`.

## Step 3 — Connect and capture

### With a `.vacf` connection config (recommended)

Write one per board and commit it next to the firmware — then connecting is
"load file, press start" in the app, or one flag on the CLI, instead of
re-entering probe settings every time. Example (`stlink-swo`):

```json
{
  "transport": "stlink-swo",
  "target-device": "STM32G474RE",
  "cpu-clock-hz": 170000000,
  "interface": "SWD",
  "speed-khz": 4000,
  "swo-freq-hz": 2000000,
  "itm-port": 1
}
```

Common keys by transport:

- all: `transport` (`stlink-swo` | `stlink-rambuf` | `jlink-rtt` | `udp` | `serial`),
  `target-device`, `interface`, `speed-khz`, `cpu-clock-hz`, `no-reset`
- `stlink-swo`: `swo-freq-hz` (must match the firmware's SWO clock), `itm-port`
- `stlink-rambuf`: `rambuf-scan-start`, `rambuf-scan-size`, `rambuf-poll-ms`
- `jlink-rtt`: `rtt-channel`, and optionally `rtt-address` (pin the control
  block from the ELF's `_SEGGER_RTT` symbol) and `jlink-serial` (only needed
  with multiple probes attached — do not commit machine-specific values)

Every example project in this repo ships its `.vacf` files; copy the closest
one and adjust.

### Headless CLI

```bash
# record 10 s to a .vadb
ViewAlyzer --headless --config my_board.vacf --output run.vadb --duration 10

# validate a recording
ViewAlyzer --headless --replay run.vadb

# JSON summary (for scripts / AI agents)
ViewAlyzer --headless --query timeline --tier summary --recording run.vadb
```

A capture that "succeeds" but decodes nothing is the classic
transport-mismatch symptom — always check the reported event count, not just
the exit code.

## Verification checklist

1. Firmware builds with the recorder sources and one transport selected.
2. `VA_Init()` runs after clock init (add a short `k_msleep`/delay before it
   if the debugger needs time to attach).
3. One user trace registered and logged (`VA_RegisterUserTrace` +
   `VA_LogTrace`) — prove the pipe with one value before adding more.
4. Host config matches the firmware transport (SWO frequency, RTT channel,
   RAM-buffer scan range).
5. Capture shows events and named threads; empty captures → transport
   mismatch, stale firmware, or (after reflashing without reset) a stale RTT
   control block from the previous image.

## Machine-specific paths

Never commit absolute tool paths. The build scripts in this repo resolve
Zephyr / toolchain / OpenOCD / J-Link locations from gitignored local files —
see [`tools.local.example.json`](tools.local.example.json):

- `build.local[.win|.linux|.mac].json` next to a project's `build.py`
  (highest priority), or
- `tools.local[.win|.linux|.mac].json` once at the repo root, then
- environment variables, then auto-detection of standard install locations.

The per-OS variants let a dual-boot machine share one checkout. Follow the
same pattern in your own projects.
