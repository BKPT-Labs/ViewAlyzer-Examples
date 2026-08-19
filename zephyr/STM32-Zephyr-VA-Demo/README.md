# STM32 Zephyr ViewAlyzer Demo

A multi-board Zephyr application that shows what the ViewAlyzer Recorder can
capture: thread scheduling, mutex contention, message queues, semaphores,
events, work queues, heap activity, timers, and user-defined traces — all in
one firmware image, so every view in the ViewAlyzer app has live data.

- **New to ViewAlyzer?** Start with [Quickstart](#quickstart) below — the whole
  integration is five lines of config and three lines of code.
- **Adding ViewAlyzer to your own Zephyr app?** Follow
  [TUTORIAL.md](TUTORIAL.md), a step-by-step integration guide.

Supported boards:

| Alias | Zephyr board    | Hardware          | Default transport |
| ----- | --------------- | ----------------- | ----------------- |
| `g4`  | `nucleo_g474re` | Nucleo-G474RE     | ITM/SWO           |
| `f4`  | `nucleo_f446re` | Nucleo-F446RE     | SEGGER RTT        |
| `h5`  | `nucleo_h503rb` | Nucleo-H503RB     | ITM/SWO           |
| `h7`  | `stm32h750b_dk` | STM32H750B-DK     | ITM/SWO           |

## Quickstart

The demo firmware is deliberately busy, but the ViewAlyzer integration itself
is tiny. To add the recorder to any Zephyr app you need exactly this:

`CMakeLists.txt` — point Zephyr at the recorder module (before
`find_package(Zephyr ...)`):

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES path/to/ViewAlyzerRecorder)
```

`prj.conf` — enable the module:

```conf
CONFIG_VIEWALYZER=y
CONFIG_TRACING_USER=y
CONFIG_VIEWALYZER_TRANSPORT_ITM=y   # or ..._RTT=y
```

`main.c` — initialize and log something:

```c
#include "ViewAlyzer.h"

VA_Init(SystemCoreClock);
VA_RegisterUserTrace(1, "Counter", VA_USER_TYPE_COUNTER);
VA_LogTrace(1, counter);
```

Thread switches, ISRs, and sync-object activity are traced automatically by
the Zephyr adapter — the calls above are only needed for your own values.
This project is that integration plus a firmware playground rich enough to
exercise every view.

## Requirements

- A Zephyr workspace (this demo targets Zephyr v4.1.0) with the `cmsis`,
  `hal_stm32`, and — for RTT — `segger` modules fetched.
- `west` on your PATH (`pip3 install --user west`).
- An Arm GNU toolchain. The build script auto-detects an
  [STM32CubeCLT](https://www.st.com/en/development-tools/stm32cubeclt.html)
  install on Windows, macOS, and Linux, or honors
  `GNUARMEMB_TOOLCHAIN_PATH`.
- The `ViewAlyzerRecorder` module, cloned next to this repo as part of the
  `ViewAlyzer` repository (or point `VIEWALYZER_MODULE_PATH` somewhere else).
- For J-Link flashing: the SEGGER J-Link software pack plus the Python
  package `pylink-square`. On Windows, `menuconfig` also needs
  `windows-curses`.

## First Build and Capture

1. Tell the build where Zephyr lives (pick one):
   - set the `ZEPHYR_BASE` environment variable, or
   - create `build.local.json` next to `build.py`:
     `{"zephyr_base": "/path/to/zephyrproject/zephyr"}` (this file is
     gitignored — safe for machine-specific paths), or
   - keep a `zephyr/` checkout in a directory above this project and let the
     script find it.
2. Build and flash (Nucleo-G474RE over its onboard ST-LINK shown):

   ```bash
   python3 build.py flash g4
   ```

3. Capture with the matching connection config:

   - **ViewAlyzer app:** open `nucleo_g474_zephyr_swo.vacf` and start the
     capture.
   - **Command line:**

     ```bash
     ViewAlyzer --headless --config nucleo_g474_zephyr_swo.vacf \
                --output demo.vadb --duration 10
     ```

You should immediately see ~20 threads scheduling, four live charts (sine
wave, shared accumulator, tick counter, mutex wait time), queue depth ramping
in bursts, and periodic mutex contention between three priorities.

Connection configs included: `nucleo_g474_zephyr_swo.vacf` and
`nucleo_h5_zephyr_swo.vacf` (each also has a `..._varambuf.vacf` RAM-buffer
variant), `nucleo_f446re_zephyr_rtt.vacf`, `stm32h750b_zephyr_swo.vacf`.

## build.py Reference

```bash
python3 build.py [action] [board] [runner] [--swo | --rtt | --varambuf | --snapshot]
```

- `action`: `build` (default), `clean` (pristine rebuild), `flash`, `debug`,
  `menuconfig`
- `board`: `g4` (default), `f4`, `h5`, `h7` (full Zephyr board names also
  accepted)
- `runner` (flash/debug only): `jlink` or `openocd` (`stlink`/`st` are
  aliases for `openocd`). Defaults: `jlink` for `f4`, `openocd` for the
  boards with an onboard ST-LINK (`g4`, `h5`, `h7`).

Examples:

```bash
python3 build.py                  # build g4
python3 build.py build h5
python3 build.py clean f4
python3 build.py flash g4         # onboard ST-LINK via OpenOCD
python3 build.py flash f4 jlink
python3 build.py debug g4
```

The script wraps `west`, resolves the toolchain/OpenOCD/J-Link installs on
all three OSes (override with `STM32CUBECLT_ROOT`, `GNUARMEMB_TOOLCHAIN_PATH`,
`OPENOCD_BIN`, `OPENOCD_SCRIPTS`, `JLINK_COMMANDER`), keeps a separate build
directory per board and transport, and cleans up stale caches copied from
another machine or OS before they can break the build.

## Transports

Each board's default transport lives in `boards/<board>.conf`. Override it
with a flag — every transport builds into its own directory, so switching
never disturbs another build:

| Flag         | Transport                | Works with                        |
| ------------ | ------------------------ | --------------------------------- |
| `--swo`      | ITM over SWO             | ST-LINK (SWO pin required)        |
| `--rtt`      | SEGGER RTT               | J-Link                            |
| `--varambuf` | ViewAlyzer RAM buffer    | any probe with memory access — plain ST-LINK is enough |
| `--snapshot` | RAM buffer, post-mortem  | run untethered, read the last trace window out later with `ViewAlyzer --snapshot` |

```bash
python3 build.py flash g4 --varambuf   # then capture with nucleo_g474_zephyr_varambuf.vacf
python3 build.py flash h5 --varambuf   # then capture with nucleo_h5_zephyr_varambuf.vacf
```

## What the Firmware Does

`src/main.c` is organized into small, independent sections — each exists to
light up a specific part of the ViewAlyzer UI (the file header has the full
map):

- a **sensor → queue → processor pipeline** that feeds the message-queue view
  with irregular bursts (depth ramps up and drains, instead of a flat line)
- a **mutex contention trio** — low/medium/high-priority threads fighting over
  one mutex, with the high-priority thread's wait time charted
- an **event-flag waiter** that sometimes times out, so failed waits show up
- a **heap demo** that periodically exhausts its pool on purpose, producing
  failed-allocation events
- **deferred work** that is scheduled, canceled, and resubmitted
- a **workload manager** that shifts CPU load between four threads every few
  seconds, so the scheduling view keeps changing shape
- user traces (`VA_RegisterUserTrace` + `VA_LogTrace`/`VA_LogTraceFloat`),
  a user event pair marking a code region, and a `VA_LogString` message

### Selective tracing: record only what you need

Every trace category is an independent Kconfig switch, and turning one off
compiles out its events, packet builders, registry storage, *and* the Zephyr
trace points that feed it — a category you don't record costs nothing at run
time. The recorder reports the set it was built with in every setup bundle,
so the ViewAlyzer app can say "mutex tracing is disabled in this firmware
build" instead of showing an empty view.

For example, to answer "which thread is blocking on which mutex, and how deep
are the stacks?" without the mutex lock/unlock traffic that dominates this
demo's stream, put a fragment like this in your `prj.conf` (or a separate
`.conf` merged via `-DEXTRA_CONF_FILE`):

```conf
# Keep
CONFIG_VIEWALYZER_TRACE_THREADS=y
CONFIG_VIEWALYZER_STACK_USAGE=y
CONFIG_VIEWALYZER_TRACE_ISRS=y
CONFIG_VIEWALYZER_TRACE_MUTEX_CONTENTION=y
CONFIG_VIEWALYZER_TRACE_USER_VALUES=y
CONFIG_VIEWALYZER_TRACE_USER_EVENTS=y

# Drop
CONFIG_VIEWALYZER_TRACE_MUTEXES=n
CONFIG_VIEWALYZER_TRACE_SEMAPHORES=n
CONFIG_VIEWALYZER_TRACE_MESSAGE_QUEUES=n
CONFIG_VIEWALYZER_TRACE_SLEEP=n
CONFIG_VIEWALYZER_TRACE_TIMERS=n
CONFIG_VIEWALYZER_TRACE_HEAPS=n
CONFIG_VIEWALYZER_TRACE_STRINGS=n
```

Contention events still work with mutex give/take tracing off: the mutex and
both threads stay registered, so the contention event can still name them.

## Troubleshooting

- **OpenOCD `open failed`** — the host cannot see the ST-LINK probe (cable,
  permissions/udev on Linux, or another program holding it open).
- **J-Link `Cannot connect to J-Link`** — probe visibility problem: check
  the SEGGER software is installed and the probe enumerates as a J-Link.
- **Empty capture (0 events)** — check the transport matches the firmware
  build (`--swo` build + RTT config captures nothing), and that the SWO
  frequency in the `.vacf` matches `CONFIG_LOG_BACKEND_SWO_FREQ_HZ`.
- **Build cannot find Zephyr / toolchain** — see
  [First Build and Capture](#first-build-and-capture) step 1 and the
  environment overrides above; `build.py` prints the paths it resolved at the
  start of every build.

More integration troubleshooting (module path, Kconfig, RTT's `segger`
module requirement, SWO frequency matching) is covered at the end of
[TUTORIAL.md](TUTORIAL.md).
