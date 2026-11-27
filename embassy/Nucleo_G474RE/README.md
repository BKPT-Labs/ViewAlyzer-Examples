# Rust Embassy on NUCLEO-G474RE

All three queues have eight slots. The producer generates ten samples per burst. In one burst out of four, it tries the ninth sample once and drops it if **Samples** is full. All other sends wait for space. The consumer toggles the green PA5 LED and holds the display mutex while processing each accepted sample. A refresh task competes for that mutex and shares an ADC permit with a calibration task. Two short-lived heartbeat instances yield and finish in the same Embassy task slot.

**Sensor** samples a 3.2-second sine wave about every 16 ms. **Filter** moves its estimate a quarter of the way toward each reading and forwards it to **Controller**. In **Comms → Resources**, watch the backlog move from **Raw readings** to **Filtered data** as the filter alternates between slower and faster processing. **Setpoint** publishes a 4.8-second sine wave through an Embassy `Signal`, sweeping the target from 256 to 768 and back. The demo needs only the board.

## Build

Use sibling checkouts of `ViewAlyzer-Examples` and `ViewAlyzer` with the Embassy recorder adapter. Cargo fetches the pinned upstream Embassy Git revision shared with the recorder. The build uses the G474 CMSIS headers already in this examples repository. Install stable Rust with the Cortex-M4F target, Arm GNU GCC with newlib-nano, and probe-rs. Run from this directory so Cargo loads the board configuration.

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

In **Comms**, follow Sensor → Raw readings → Filter → Filtered data → Controller. In the trace charts, overlay **Raw input** and **Filtered input** to see the delay from queueing and filtering. **Applied target** shows the slower wave sampled by the controller. **Control error** is that target minus the filtered input and crosses zero as the input passes the target. These four graph channels use IDs 4–7. The poll-wake channels use IDs 2 and 3. **Before WFE** (8) marks each visit to the executor sleep instruction. Each Heartbeat sets priority 1 and a 25 ms deadline. **Deadline in us** (9) records the time remaining when the task assigns that deadline.

In **Runtime Map**, enable **Enhance issues** and play through a failed send to see the marker on **Samples**. Filter map activity to **Failed** to jump to an occurrence. The native analyzer attributes the failed operations to **Producer**.

```sh
viewalyzer-cli query task-states --recording g474-embassy.vadb
viewalyzer-cli query resources --recording g474-embassy.vadb
viewalyzer-cli query semaphores --recording g474-embassy.vadb
viewalyzer-cli query comms --recording g474-embassy.vadb
viewalyzer-cli query user-traces --recording g474-embassy.vadb
viewalyzer-cli map activity --recording g474-embassy.vadb --tags failed
viewalyzer-cli query verdicts --recording g474-embassy.vadb
```
