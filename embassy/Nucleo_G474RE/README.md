# Rust Embassy on NUCLEO-G474RE

The producer bursts five samples into a two-slot channel. The consumer toggles the green PA5 LED and holds the display mutex while processing each sample. A refresh task competes for that mutex and shares an ADC permit with a calibration task. Two short-lived heartbeat instances yield and finish in the same Embassy task slot.

## Build

Use sibling checkouts of `ViewAlyzer-Examples` and `ViewAlyzer` with the Embassy recorder adapter. The build uses the G474 CMSIS headers already in this examples repository. Install stable Rust with the Cortex-M4F target, Arm GNU GCC with newlib-nano, and probe-rs. Run from this directory so Cargo loads the board configuration.

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

Open the recording in ViewAlyzer. **Timeline** shows named task polls, sleeps, both heartbeat lifetimes, and the executor poll span. **Comms** shows the Producer → Samples → Consumer FIFO path, Display mutex use, and ADC permit handoffs. In the **Event Table**, filter for **Poll self-wake** to see when a Heartbeat task yields and re-enqueues itself. The sample value is its recorder task ID. **Poll IRQ wake** records an interrupt re-enqueuing the currently polled task. Timer futures appear as task sleeps. The **Timers** panel is for kernel timer objects and callbacks.

```sh
viewalyzer-cli query task-states --recording g474-embassy.vadb
viewalyzer-cli query resources --recording g474-embassy.vadb
viewalyzer-cli query semaphores --recording g474-embassy.vadb
viewalyzer-cli query comms --recording g474-embassy.vadb
```
