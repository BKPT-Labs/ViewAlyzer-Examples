# Rust Embassy on NUCLEO-G474RE

All three queues have eight slots. The producer generates ten samples per burst. In one burst out of four, it tries the ninth sample once and drops it if **Samples** is full. All other sends wait for space. The consumer toggles the green PA5 LED and holds the display mutex while processing each accepted sample. A refresh task competes for that mutex and shares an ADC permit with a calibration task. Two short-lived heartbeat instances yield and finish in the same Embassy task slot.

**Sensor** samples a 3.2-second sine wave about every 16 ms. **Filter** moves its estimate a quarter of the way toward each reading and forwards it to **Controller**. The filter alternates between slower and faster processing. **Setpoint** publishes a 4.8-second sine wave through an Embassy `Signal`, sweeping the target from 256 to 768 and back. The demo needs only the board.

## Build

Use sibling checkouts of `ViewAlyzer-Examples` and `ViewAlyzer` with the Embassy recorder adapter. On this feature branch, Cargo fetches the pinned native-hook revision of EdwinFairchild/embassy shared with the recorder. The build uses the G474 CMSIS headers already in this examples repository. Install stable Rust with the Cortex-M4F target, Arm GNU GCC with newlib-nano, and probe-rs. Run from this directory so Cargo loads the board configuration.

```sh
rustup target add thumbv7em-none-eabihf
cargo build --release --locked
```

The firmware ELF is `build/thumbv7em-none-eabihf/release/nucleo-g474-embassy`. On Windows, use a configured MSVC developer shell with its SDK libraries, or a GNU-hosted Rust toolchain with MinGW available. The tested GNU-host command is `cargo +stable-x86_64-pc-windows-gnu build --release --locked`.

## Flash and record

Replace `SERIAL` with the G474's onboard ST-LINK serial from `probe-rs list`. If the probe reports a different VID/PID, use its complete selector. Reserve shared hardware according to your bench's normal procedure.

```sh
probe-rs download --verify --chip STM32G474RETx --probe 0483:374e:SERIAL build/thumbv7em-none-eabihf/release/nucleo-g474-embassy
viewalyzer-cli capture --config nucleo_g474_embassy_rambuf.vacf --stlink-serial SERIAL --elf build/thumbv7em-none-eabihf/release/nucleo-g474-embassy --duration 8 --output g474-embassy.vadb
```

Capture resets and starts the target. The onboard ST-LINK drains a 32 KiB RAM ring over SWD. The ELF locates the recorder's buffer and metadata. The STM32 runs at 16 MHz and the recorder timestamps events using the 1 MHz Embassy time driver.

In **Timeline**, follow both Heartbeat lifetimes. Each Heartbeat sets priority 1 and a 25 ms deadline, yields once, waits on a native Embassy timer, then finishes. The adapter records the timer wait as Sleeping and the later wake as Ready. **Poll self-wake** marks a running task enqueuing itself; **Poll IRQ wake** marks an interrupt enqueuing the running task. **Before WFE** marks the thread reaching the sleep instruction. **Deadline in us** records time remaining when the deadline was assigned.

The queues, mutex, semaphore, and timers drive the workload using the native Embassy APIs. Open **Comms → Resources** to see the three eight-slot channels. In **Paths**, Producer → Consumer identifies Samples, Sensor → Filter identifies Raw readings, and Filter → Controller identifies Filtered data. The underlying labels include the traced state address. Enable **Runtime Map** before capture to see wait edges from each task to its channel, mutex or semaphore. The native Signal used by Setpoint has no hook on this branch. The four application-defined waveform traces are **Raw input**, **Filtered input**, **Applied target**, and **Control error**.

```sh
viewalyzer-cli query task-states --recording g474-embassy.vadb
viewalyzer-cli query user-traces --recording g474-embassy.vadb
```
