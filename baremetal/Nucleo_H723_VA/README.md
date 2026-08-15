# Nucleo-H723 - Bare-metal RAM Buffer Demo

Bare-metal (no RTOS) ViewAlyzer example for the **Nucleo-H723ZG**
(STM32H723ZG, Cortex-M7 @ 192 MHz) streaming over the **RAM buffer
transport**: the recorder keeps a ring buffer in RAM and the ViewAlyzer app
drains it through the on-board **ST-Link** with non-intrusive memory reads.
No SWO pin, no SEGGER RTT sources, no extra wiring - just the USB cable.

## What it traces

- `Sine Wave`, `Tick Counter`, `Workload` user traces (graph/bar)
- `Work Block` profiled span whose duration breathes over time
- `LED Toggle` toggle trace synced to the green LED
- SysTick ISR enter/exit (1 kHz timeline)
- A periodic log string

## Build & flash

```sh
cmake --preset Release          # uses cmake/gcc-arm-none-eabi.cmake
cmake --build --preset Release
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "program build/Release/Nucleo_H723_VA.elf verify reset exit"
```

The transport is a CMake cache variable - to try SWO instead:

```sh
cmake --preset Release -DVA_TRANSPORT=ARM_ITM
```

## Record with the ViewAlyzer app

**GUI:** Probe Type `ST-Link` → Trace Source `RAM Buffer` → Connect.
The app scans RAM for the recorder's control block automatically (defaults
`0x20000000` + 128 KB cover this project's DTCM placement).

**Headless CLI** - use the connection config shipped with this project:

```sh
ViewAlyzer --headless --config nucleo_h723_rambuf.vacf \
           --output h723_demo.vadb --duration 10
```

`nucleo_h723_swo.vacf` is the matching config for the ITM/SWO build
(`-DVA_TRANSPORT=ARM_ITM`). In the GUI, both load via the **Load Config**
button. Adjust the `arm-gdb` path inside if your tools live elsewhere.

Flags always win over config values. Useful extras: `--rambuf-address
<0xADDR>` (skip the scan - the address of the `_VA_RAMBUF` symbol in your
`.map` file), `--rambuf-scan-start` / `--rambuf-scan-size` to move the scan
window, `--rambuf-poll-ms <n>`.

## Notes

- The ring buffer defaults to 8 KB (`VA_RAMBUF_SIZE`) and drops whole
  packets when full (`VA_RAMBUF_MODE_DROP`) - the drop counter is visible to
  the host, and the stream stays parseable.
- Caches are left disabled by this template, so debugger reads are always
  coherent. If you enable the D-cache, place the buffer in a non-cacheable
  MPU region via `VA_RAMBUF_ATTRIBUTES`.
