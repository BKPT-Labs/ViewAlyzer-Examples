# ViewAlyzer on the NUCLEO-L031K6 (Zephyr)

The **8 KB RAM floor test**: the STM32L031K6 (Cortex-M0+) is the smallest
recorder target so far. Same constraints as the C031 example (no DWT, no
ITM, no 32-bit timer) with every size knob trimmed to fit.

| What | How on this board |
|---|---|
| Timestamps | TIM2 (16-bit) free-running at 100 kHz, `CONFIG_VIEWALYZER_TIMER_BITS=16` |
| Transport | ViewAlyzer RAM buffer (1 KB), drained by the on-board ST-LINK |
| Footprint | ~22 KB flash / ~6.3 KB RAM of the L031's 32 KB / 8 KB |

If you add threads or traces here, budget RAM first: the link fails loudly
when it no longer fits (which is the good failure mode).

Everything else works like the
[NUCLEO-G0B1RE example](../Nucleo-G0B1RE-Zephyr-VA-Recorder/README.md).

```bash
python3 build.py            # build
python3 build.py flash      # flash via the on-board ST-LINK
ViewAlyzer --headless --config nucleo_l031k6_zephyr_varambuf.vacf \
           --output capture.vadb --duration 10
```

With several ST-LINKs attached, add `--stlink-serial <sn>`
(`ViewAlyzer --headless --list-probes` lists them).
