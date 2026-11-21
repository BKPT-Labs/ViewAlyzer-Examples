// SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
#![no_std]
#![no_main]

use cortex_m_rt::entry;
use embassy_executor::{Executor, Spawner};
use embassy_stm32::gpio::{Level, Output, Speed};
use embassy_sync::{
    blocking_mutex::raw::CriticalSectionRawMutex, channel::Channel, mutex::Mutex,
    semaphore::GreedySemaphore,
};
use embassy_time::Duration;
use panic_halt as _;
use viewalyzer_embassy::{Traced, sleep, spawn_named};

type Raw = CriticalSectionRawMutex;
type Samples = Channel<Raw, u32, 2>;
type Display = Mutex<Raw, u32>;
type Slot = GreedySemaphore<Raw>;

static SAMPLES: Samples = Channel::new();
static DISPLAY: Display = Mutex::new(0);
static SLOT: Slot = GreedySemaphore::new(1);

#[entry]
fn main() -> ! {
    let p = embassy_stm32::init(Default::default());
    let hz = embassy_stm32::rcc::clocks(&p.RCC)
        .hclk1
        .to_hertz()
        .unwrap()
        .0;
    let led = Output::new(p.PA5, Level::Low, Speed::Low);
    let executor = cortex_m::singleton!(: Executor = Executor::new()).unwrap();
    executor.run(|spawner| {
        viewalyzer_embassy::init(spawner, hz, 1);
        let samples = Traced::channel(&SAMPLES, c"Samples");
        let display = Traced::mutex(&DISPLAY, c"Display");
        let slot = Traced::semaphore(&SLOT, c"ADC permit");
        spawn_named(spawner, producer(samples, spawner).unwrap(), c"Producer");
        spawn_named(
            spawner,
            consumer(samples, display, led).unwrap(),
            c"Consumer",
        );
        spawn_named(spawner, refresh(display, slot).unwrap(), c"Refresh");
        spawn_named(spawner, calibrate(slot).unwrap(), c"Calibration");
    })
}

#[embassy_executor::task]
async fn producer(samples: Traced<Samples>, spawner: Spawner) {
    let mut sequence = 0u32;
    loop {
        // Burst past channel capacity, then leave the consumer waiting for input.
        for _ in 0..5 {
            samples.send(sequence).await;
            sequence = sequence.wrapping_add(1);
        }
        if sequence == 25 || sequence == 50 {
            spawn_named(spawner, heartbeat().unwrap(), c"Heartbeat");
        }
        sleep(Duration::from_millis(180)).await;
    }
}

#[embassy_executor::task]
async fn heartbeat() {
    embassy_futures::yield_now().await;
    sleep(Duration::from_millis(10)).await;
}

#[embassy_executor::task]
async fn consumer(samples: Traced<Samples>, display: Traced<Display>, mut led: Output<'static>) {
    loop {
        let sample = samples.receive().await;
        let mut guard = display.lock().await;
        *guard = sample;
        led.toggle();
        sleep(Duration::from_millis(18)).await;
    }
}

#[embassy_executor::task]
async fn refresh(display: Traced<Display>, slot: Traced<Slot>) {
    loop {
        {
            let _permit = slot.acquire().await;
            let _display = display.lock().await;
            sleep(Duration::from_millis(25)).await;
        }
        sleep(Duration::from_millis(30)).await;
    }
}

#[embassy_executor::task]
async fn calibrate(slot: Traced<Slot>) {
    loop {
        {
            let _permit = slot.acquire().await;
            sleep(Duration::from_millis(15)).await;
        }
        sleep(Duration::from_millis(28)).await;
    }
}
