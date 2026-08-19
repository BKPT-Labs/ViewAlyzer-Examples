# Nucleo-C031C6 - ViewAlyzer Bare-metal Example (Cortex-M0+)

The smallest-core integration reference: STM32C031C6 (Cortex-M0+ @ 48 MHz,
32 KB flash, 12 KB SRAM), no RTOS. It shows the two things a Cortex-M0+
needs that bigger cores get for free:

- **No DWT cycle counter** - timestamps come from a free-running timer
  (TIM3, 1 MHz, 16-bit) through the recorder's `CUSTOM_TIMER` source
  (`Core/Inc/va_config.h`, `VA_Init(cpu_hz, tick_read, tick_hz)`), and the
  main loop calls `VA_TickOverflowCheck()` every pass because a 16-bit tick
  wraps every 65 ms.
- **No ITM / SWO** - the transport is the RAM buffer (4 KB ring) the
  on-board ST-LINK V2-1 drains with non-intrusive memory reads. Post-mortem
  snapshot capture works too.

What the core does have is `DWT_PCSR`, so the ViewAlyzer app can sample the
program counter by debug-port polling (~700 samples/s over the V2-1) for a
live statistical profile: enable Hardware Trace (DWT) with PC Samples set,
open the Profiler panel, Connect. The `.vacf` here does that already.

The firmware runs a layered integer DSP chain every loop pass (sensor ->
filter -> envelope -> classify -> stats), so a PC sample always lands in real
code with real call depth, and logs decimated user traces on top: two graphs,
a toggle, a bar, a periodic log string, and a `Work Block` event span. SysTick
is instrumented as an ISR band.

## Build & flash

```sh
python3 build.py --flash                  # RAM buffer (the only transport on this part)
# --serial <probe-serial> picks a probe when several are connected
```

## Record

Open `nucleo_c031_rambuf.vacf` in the ViewAlyzer app (Load Config), set the
ELF (`build/rambuf/Nucleo_C031_VA.elf`) under ELF / Symbols, open the
Profiler panel and Connect - or headless:

```sh
ViewAlyzer --headless --config nucleo_c031_rambuf.vacf \
           --elf build/rambuf/Nucleo_C031_VA.elf \
           --output demo.vadb --duration 10
ViewAlyzer --headless --query profile --recording demo.vadb \
           --elf build/rambuf/Nucleo_C031_VA.elf      # hotspots
```

Memory: ~7 KB RAM (4 KB ring + recorder state + stack), ~6.5 KB flash.
