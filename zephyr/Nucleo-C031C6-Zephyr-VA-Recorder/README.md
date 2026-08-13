# ViewAlyzer on the NUCLEO-C031C6 (Zephyr)

The **16-bit timer-source reference**: the STM32C031C6 (Cortex-M0+) has no
DWT, no ITM, and **no 32-bit timer**, so this example runs the recorder's
`CUSTOM_TIMER` source with `CONFIG_VIEWALYZER_TIMER_BITS=16`.

| What | How on this board |
|---|---|
| Timestamps | TIM3 (16-bit) free-running at 100 kHz (10 us resolution) |
| Wrap budget | 655 ms per wrap; events + a 50 ms `VA_TickOverflowCheck()` cadence give a >13x margin |
| Transport | ViewAlyzer RAM buffer (2 KB), drained by the on-board ST-LINK |
| Footprint | ~22 KB flash / ~9 KB RAM of the C031's 32 KB / 12 KB |

Everything else works like the
[NUCLEO-G0B1RE example](../Nucleo-G0B1RE-Zephyr-VA-Recorder/README.md)
(same `VA_Init(cpu_freq, ts_fn, tick_hz)` wiring, same ERR:TS_DEAD failure
demo by commenting out `tick_timer_init()`).

```bash
python3 build.py            # build
python3 build.py flash      # flash via the on-board ST-LINK
ViewAlyzer --headless --config nucleo_c031c6_zephyr_varambuf.vacf \
           --output capture.vadb --duration 10
```

With several ST-LINKs attached, add `--stlink-serial <sn>`
(`ViewAlyzer --headless --list-probes` lists them).
