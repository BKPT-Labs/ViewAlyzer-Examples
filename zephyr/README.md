# Zephyr Example Projects

Standalone Zephyr applications that use the ViewAlyzer Recorder as an
external Zephyr module. They expect a Zephyr checkout on disk; see
[../README.md](../README.md#zephyr) for the recommended layout.

| Project | Boards | Notes |
|---------|--------|-------|
| [STM32-Zephyr-VA-Demo](STM32-Zephyr-VA-Demo) | `nucleo_g474re`, `nucleo_f446re`, `nucleo_h503rb`, `stm32h750b_dk` | **Start here.** The full demo: every traceable kernel object, multi-board, multi-transport, with a `build.py` west wrapper, a quickstart, and a step-by-step [TUTORIAL](STM32-Zephyr-VA-Demo/TUTORIAL.md) for integrating into your own app. |
| [MAX32657-Zephyr-VA-Recorder](MAX32657-Zephyr-VA-Recorder) | `max32657evkit` | ADI Cortex-M33 over the on-board J-Link OB (SEGGER RTT). |
| [Nucleo-G0B1RE-Zephyr-VA-Recorder](Nucleo-G0B1RE-Zephyr-VA-Recorder) | `nucleo_g0b1re` | **Cortex-M0+** (no DWT, no ITM): timestamps from TIM2 (32-bit) via the `CUSTOM_TIMER` source, RAM-buffer transport over the on-board ST-LINK. |
| [Nucleo-C031C6-Zephyr-VA-Recorder](Nucleo-C031C6-Zephyr-VA-Recorder) | `nucleo_c031c6` | **16-bit timer reference** (C031 has no 32-bit timer): TIM3 at 100 kHz with `VIEWALYZER_TIMER_BITS=16`, trimmed for 12 KB RAM. |
| [Nucleo-L031K6-Zephyr-VA-Recorder](Nucleo-L031K6-Zephyr-VA-Recorder) | `nucleo_l031k6` | **8 KB RAM floor test**: 16-bit TIM2 tick, 1 KB ring, smallest working recorder target. |

The demo is the tutorial project; the three Nucleo M0+ projects are porting
references for constrained hardware (no DWT cycle counter, 16-bit timers,
single-digit KB of RAM) — copy whichever matches your part's constraints.

## Expected Layout

These projects locate two things on disk:

- **The recorder module** (`ViewAlyzerRecorder`): resolved by each project's
  `CMakeLists.txt` — a `-DVIEWALYZER_MODULE_PATH=...` option, the
  `VIEWALYZER_MODULE_PATH` environment variable, or a `ViewAlyzer` checkout
  next to this repo, in that order.
- **Zephyr itself**: the `ZEPHYR_BASE` environment variable, a per-machine
  `build.local.json` next to the project's `build.py` (demo project), or a
  `zephyr/` directory found in a parent directory.

The recommended layout satisfies both with two sibling clones:

```
<your-workspace>/
├── ViewAlyzer/                  # recorder module lives in ViewAlyzerRecorder/
├── ViewAlyzer-Examples/         # this repository
│   └── zephyr/<project>/
└── zephyr/                      # Zephyr source tree (west.yml, kernel/, ...)
```

Build (typical):

```bash
cd zephyr/STM32-Zephyr-VA-Demo
python3 build.py            # default G4 build
python3 build.py flash g4   # build + flash via the onboard ST-LINK
```
