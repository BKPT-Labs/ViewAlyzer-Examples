# MAX32657EVKIT Zephyr ViewAlyzer Recorder

A standalone Zephyr application that pulls in the **ViewAlyzer Recorder** as an
external module and streams traces off an **Analog Devices MAX32657EVKIT**
(Cortex-M33, BLE 5.4) through the board's **on-board J-Link OB** via **SEGGER
RTT** — no SWO pin and no external probe required.

It blinks the LED, logs a few user traces, and runs a second worker thread that
contends on a mutex, so thread-switch and mutex tracing have something to show
on the ViewAlyzer timeline.

## What's in here

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Adds `ViewAlyzerRecorder` via `ZEPHYR_EXTRA_MODULES` |
| `prj.conf` | Enables `CONFIG_VIEWALYZER` + `CONFIG_TRACING_USER`, selects RTT on channel 1 |
| `src/main.c` | Init recorder, blink LED, log Sine/Uptime/Work traces, worker thread + mutex |
| `build.py` | Optional helper that sets up the toolchain env and drives `west` |
| `max32657evkit_jlink_rtt.vacf` | ViewAlyzer host config for the desktop/CLI app |

That's the whole integration — the recorder sources come from the ViewAlyzer
repo's `ViewAlyzerRecorder` module (cloned next to this repo, or pointed at
via `VIEWALYZER_MODULE_PATH`).

## Hardware

- ADI `max32657evkit/max32657` (the secure Zephyr board target)
- On-board J-Link OB (enumerates as a SEGGER device — no separate probe needed)

Core clock is the 50 MHz internal primary oscillator (IPO); the recorder's
timestamps come from the M33 DWT cycle counter at that rate.

## Requirements

- A Zephyr workspace with `zephyr/` checked out **and the ADI HAL fetched**:
  ```bash
  west update hal_adi          # provides wrap_max32xxx.h / the MAX32 SoC support
  ```
- An Arm toolchain Zephyr can find — either the Zephyr SDK, or a generic
  `arm-none-eabi` GCC via `ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb` +
  `GNUARMEMB_TOOLCHAIN_PATH` (a stock STM32CubeCLT toolchain works for the M33).
- The `segger` module in your west manifest (already pulled by a normal
  `west update`) — RTT needs it.
- SEGGER J-Link software (see the flashing note below).

## Build

With the Zephyr SDK on your PATH, the plain west flow works:

```bash
west update hal_adi
west build -b max32657evkit/max32657 .
```

If your Zephyr SDK is missing/incompatible and you want to use a generic
`arm-none-eabi` GCC instead, either export the env yourself:

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/path/to/GNU-tools-for-STM32
west build -b max32657evkit/max32657 .
```

…or just use the helper, which resolves all of that automatically:

```bash
python3 build.py            # build
python3 build.py flash      # build + west flash --runner jlink
```

If the ViewAlyzer repo is not cloned next to this examples repo, point CMake
at the recorder directly:

```bash
west build -b max32657evkit/max32657 . -- \
  -DVIEWALYZER_MODULE_PATH=/absolute/path/to/ViewAlyzerRecorder
```

## Flash

```bash
west flash --runner jlink            # or: python3 build.py flash
```

> **⚠️ J-Link version matters for this (still pre-release) part.**
> The MAX32657 is only in **current** SEGGER J-Link software. If flashing pops
> a *"The selected device MAX32657 is unknown to this version of the J-Link
> software"* dialog, your J-Link pack is too old — upgrade the **J-Link
> Software & Documentation Pack** from segger.com. The MAX32657 has an unusual
> flash base (`0x01000000`), so there is no safe substitute device: the J-Link
> flash loader must actually know the part.

## Verify in the ViewAlyzer app

The recorder publishes on **RTT channel 0** (the channel J-Link's GDB-server RTT
socket streams). The supplied config uses a passive attach (`no-reset: true`) so
the recorder has already initialized its RTT buffer when the host attaches:

```bash
ViewAlyzer --headless --transport jlink-rtt \
  --config max32657evkit_jlink_rtt.vacf --output cap.vadb --duration 8
```

> The config pins the RTT control block address (`rtt-address`, taken from the
> ELF's `_SEGGER_RTT` symbol — update it after builds that move `.bss`). If you
> have more than one J-Link attached, add a `"jlink-serial"` entry so the host
> picks the right probe.

Confirm the recorder is alive independently of the app by reading the RTT block
over SWD (address from `arm-none-eabi-nm build/zephyr/zephyr.elf | grep _SEGGER_RTT`,
~`0x30000410`):

```
JLinkExe -nogui 1  ->  device MAX32657 / connect / mem8 0x30000410 16
# expect: 53 45 47 47 45 52 20 52 54 54 00  ("SEGGER RTT")
```

A capture shows thread switches (`main`, `worker_tid`, `idle`, …), the
`Sine`/`Uptime`/`Work` user traces, and mutex events. Expect roughly 2 KB/s
through the on-board J-Link OB (a standalone J-Link sustains far higher rates
on the same path).

## Switching transport

RTT is the default because it needs no extra wiring through the J-Link OB. The
recorder also supports ITM/SWO (`CONFIG_VIEWALYZER_TRANSPORT_ITM=y`) if you
route SWO — see the recorder's Zephyr README
(`ViewAlyzerRecorder/zephyr/README.md` in the ViewAlyzer repo) for the full
Kconfig surface.
