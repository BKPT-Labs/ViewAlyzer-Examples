# Nucleo-C071RB - ViewAlyzer Bare-metal Example with a synthetic IMU (Cortex-M0+)

STM32C071RB (Cortex-M0+ @ 48 MHz, 128 KB flash, 24 KB SRAM), no RTOS. A
clone of the Nucleo-C031C6 example with one addition: a **synthetic 6-channel
IMU** whose `imu.*` user traces pair with the `com.bkpt.demo.imu` Trace
Domain descriptor in ViewAlyzer. It is the reference for how an OEM data
domain reaches the app without any OEM code in the firmware or the app:

- The firmware emits plain user traces (`imu.ax/ay/az`, `imu.heading`,
  `imu.temp`, `imu.motion`) with plain GRAPH/TOGGLE hints.
- The `.vadomain` descriptor claims the `imu.*` channels host-side and adds
  what the firmware cannot express: units (m/s2, deg, degC), channel groups,
  and display overrides (heading becomes an angle dial, temp a gauge).

The Cortex-M0+ specifics carried over from the C031 example:

- **No DWT cycle counter** - timestamps come from a free-running timer
  (TIM3, 1 MHz, 16-bit) through the recorder's `CUSTOM_TIMER` source
  (`Core/Inc/va_config.h`, `VA_Init(cpu_hz, tick_read, tick_hz)`), and the
  main loop calls `VA_TickOverflowCheck()` every pass because a 16-bit tick
  wraps every 65 ms.
- **No ITM / SWO** - the transport is the RAM buffer (8 KB ring) the
  on-board ST-LINK drains with non-intrusive memory reads. Post-mortem
  snapshot capture works too.
- `DWT_PCSR` exists, so the app can still sample the program counter by
  debug-port polling for a live statistical profile; the DSP workload chain
  (sensor -> filter -> envelope -> classify -> stats) runs every loop pass
  so samples land in real code.

The IMU is synthesized on-chip (no sensor wired to the board): a slowly
yawing, gently tilting body with LFSR-triggered shake bursts. `imu.motion`
toggles during a burst and `imu.temp` shows a small self-heating offset, so
the channels correlate visibly.

## Build & flash

```sh
python3 build.py --flash                  # RAM buffer (the only transport on this part)
# --serial <probe-serial> picks a probe when several are connected
```

## Record

ViewAlyzer-RS (the GPUI app / `viewalyzer-cli`) with the demo descriptor
installed (`viewalyzer-cli domains` shows the search dirs; during
development set `VA_DOMAINS_DIR=<ViewAlyzer-RS>/domains`):

```sh
viewalyzer-cli capture --config nucleo_c071_rambuf.vacf \
                       --elf build/rambuf/Nucleo_C071_VA.elf \
                       --output imu-demo.vadb --duration 10
viewalyzer-cli load imu-demo.vadb          # channels_detail shows the imu.* rows
```

The capture log prints `Trace Domain com.bkpt.demo.imu 0.1.0: claimed N
channel(s)`, and the recording's meta carries `va_domains`. In the GUI,
open the Trace panel: the `imu.*` charts come up grouped with units, the
heading as an angle dial and the temperature as a gauge.

The classic C++ app records this target the same way as the C031 example
(`Load Config`, set the ELF, Connect); it simply shows the plain traces.

## External instrument sync (any instrument)

The firmware also emits **sync marks**: a rising edge on PC10 (morpho
connector CN7 pin 1) paired with a `VA_LogTrace("jsync", seq)` event, at
LFSR-dithered intervals. Wire that
pin + GND to anything that can timestamp a logic edge on its own clock
(power analyzer GPI, DAQ, logic analyzer), convert its capture to the
external-series NDJSON contract (`ViewAlyzer-RS/docs/EXTERNAL-SERIES.md`),
and merge:

```sh
viewalyzer-cli import imu-demo.vadb --series instrument.ndjson --max-resid-us 500
# or in one step, spawning the instrument's recorder for the capture window:
viewalyzer-cli capture --config nucleo_c071_rambuf.vacf --elf build/rambuf/Nucleo_C071_VA.elf \
    --output imu-demo.vadb --duration 15 \
    --instrument-cmd "<whatever records your instrument>" --instrument-series instrument.ndjson
```

The import reports matched pairs, clock drift and fit residuals. Note the
LFSR restarts from a fixed seed at reset, so every boot emits the same
fingerprint: always import the instrument file recorded alongside THAT
capture. For the Joulescope specifically, the adapter is
`joulescope-demo/scripts/npz_to_vaext.py`.

Memory: ~11 KB RAM (8 KB ring + recorder state + stack), ~7 KB flash.
