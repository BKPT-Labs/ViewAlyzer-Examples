# ViewAlyzer on the NUCLEO-G0B1RE (Zephyr)

Full ViewAlyzer tracing on a **Cortex-M0+**: the first example for a core
with **no DWT cycle counter and no ITM**.

| What | How on this board |
|---|---|
| Timestamps | TIM2 (32-bit) free-running at 1 MHz, via the recorder's `CUSTOM_TIMER` source |
| Transport | ViewAlyzer RAM buffer, drained by the **on-board ST-LINK** (no SWO, no SEGGER code) |
| Board | ST NUCLEO-G0B1RE (STM32G0B1RE, 144 KB RAM / 512 KB flash) |

## How the custom timestamp source works

ARMv6-M has no `DWT->CYCCNT`, so `prj.conf` selects
`CONFIG_VIEWALYZER_TS_CUSTOM_TIMER=y` and `src/main.c`:

1. starts TIM2 as a free-running 32-bit counter prescaled to 1 MHz;
2. hands it to the recorder - with the custom source, `VA_Init()` takes
   the tick function and its rate directly:

   ```c
   tick_timer_init();                    /* timer MUST run before VA_Init */
   VA_Init(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC, tick_timer_read, TICK_HZ);
   ```

The recorder reports the tick rate to the host (the `CLK:` info packet),
so timeline durations are correct even though the tick rate (1 MHz) is
nothing like the CPU clock (64 MHz). Timestamp resolution is one tick:
1 us here. Raise `TICK_HZ` for finer resolution if you need it.

**Seeing the failure path:** comment out `tick_timer_init()` and rebuild.
The recorder detects the dead counter at init, reports `ERR:TS_DEAD` to
the host (visible in the ViewAlyzer GUI log and in `--headless` output),
and refuses to start rather than producing a capture full of frozen
timestamps.

## Build and flash

```bash
python3 build.py            # build
python3 build.py flash      # flash via the on-board ST-LINK (STM32CubeProgrammer)
```

The script resolves Zephyr (`ZEPHYR_BASE` or a sibling checkout) and the
Arm toolchain / STM32_Programmer_CLI from an STM32CubeCLT install; see the
docstring in `build.py`.

## Capture

GUI: open `nucleo_g0b1re_zephyr_varambuf.vacf` and record.

Headless:

```bash
ViewAlyzer --headless --config nucleo_g0b1re_zephyr_varambuf.vacf \
           --record capture.vadb --duration 10
```

The host finds the RAM ring by scanning for its magic tag; to skip the
scan, pin `"rambuf-address"` to the `_VA_RAMBUF` symbol address from
`build/zephyr/zephyr.elf`.

## Files

| File | Purpose |
|---|---|
| `src/main.c` | Demo workload + TIM2 tick source + `VA_Init` wiring |
| `prj.conf` | Recorder config: RAM-buffer transport, custom timer source |
| `build.py` | Build/flash helper (gnuarmemb + STM32CubeProgrammer via CubeCLT) |
| `nucleo_g0b1re_zephyr_varambuf.vacf` | Host capture config (ST-LINK rambuf) |
